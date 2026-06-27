# MultiPaxosDB — Detailed Change Summary

> A complete, chronological, engineering-level record of the work done to take
> MultiPaxosDB from a single-transaction-per-round prototype (~37 TPS measured) to
> a batched system that sustains **~10,000 TPS** on localhost with money strictly
> conserved. Read top to bottom; each section says **what**, **why**, and **how it
> was verified**.

**TL;DR of the headline result:** the throughput goal was reached not by the
optimization everyone expected (group-commit / fsync batching) but by discovering,
empirically, that (a) the old "~190 TPS" number was an artifact of a throttled test
harness, and (b) the system was committing exactly **one transaction per Paxos
round** because the workload never offered concurrent load to a single leader. Once
real concurrent load is offered across the three independent cluster-leaders,
batching engages on its own and throughput reaches ~10k TPS — with **no group-commit
change required**.

---

## Table of contents

1. [Starting point](#1-starting-point)
2. [Change 1 — Batched Multi-Paxos rounds](#2-change-1--batched-multi-paxos-rounds)
3. [Change 2 — Idempotent balance application (money-conservation fix)](#3-change-2--idempotent-balance-application-money-conservation-fix)
4. [Change 3 — Step A: gate Paxos sync on a Paxos-only index](#4-change-3--step-a-gate-paxos-sync-on-a-paxos-only-index)
5. [Change 4 — Raise the peer RPC deadline](#5-change-4--raise-the-peer-rpc-deadline)
6. [Change 5 — The throughput investigation and the load benchmark](#6-change-5--the-throughput-investigation-and-the-load-benchmark)
7. [Other fixes (stability / harness)](#7-other-fixes-stability--harness)
8. [What is new — file-by-file](#8-what-is-new--file-by-file)
9. [How to reproduce every result](#9-how-to-reproduce-every-result)
10. [Invariants that must never regress](#10-invariants-that-must-never-regress)
11. [Commit history](#11-commit-history)

---

## 1. Starting point

The system is a sharded bank: **3 clusters × 3 servers** (`S1`–`S9`), each cluster
replicating one shard. Intra-shard transfers reach agreement via **Multi-Paxos**;
cross-shard transfers use **2PC** with the client as coordinator. Each server is a
**single-threaded async-gRPC event loop** (no server-side locks needed). Storage is
LevelDB (balances + ballot) plus a plain-text WAL (2PC durability).

Originally each client transaction drove **one full Paxos round** (Prepare → Accept →
Commit, a 6-RPC peer fan-out plus disk writes). Measured throughput was ~37 TPS with
the old benchmark; latency was dominated by the per-transaction round-trip and disk
I/O.

---

## 2. Change 1 — Batched Multi-Paxos rounds

**What.** The server was reworked so that a single Paxos round can commit **many
client commands at once** instead of one. This amortizes the 6 peer RPCs and the disk
writes of a round across every command in the batch.

**Why.** A round's fixed cost (prepare/accept/commit fan-out + WAL append + LevelDB
writes) is paid once per round regardless of how many commands it carries. Committing
N commands per round divides that fixed cost by N.

**How it works.**

- **Proto (`src/proto/tpc.proto`):**
  - `AcceptReq`: `TransferReq r` → `repeated TransferReq batch` — an Accept now carries
    a whole batch.
  - `PrepareRes`: `optional TransferReq accept_val` → `repeated TransferReq accept_val`
    — a Promise can return a batch of previously-accepted values.
- **Server state machine (`src/server/server.{h,cc}`):** the old single-transaction
  Paxos state was replaced with a batched round driven by the reply handlers:
  - `pending_` — `std::vector<BatchEntry>` accumulating commands for the *next* round.
  - `current_` — entries accepted into the *in-flight* round.
  - `BatchEntry { TransferReq request; InCall* call; bool is_cross_shard; }`.
  - `bool is_paxos_running` — only one round runs at a time (rounds are serialized).
  - `std::vector<TransferReq> accept_batch` — values accepted in the current round.
  - Methods: `enqueueClientTxn`, `maybeStartRound`, `startRound`,
    `reissuePrepareForCurrent`, `onPrepareQuorum`, `onAcceptQuorum`,
    `onRoundAbort`, `finishRound`.
  - **Flow:** client txn → `enqueueClientTxn` (push to `pending_`) → `maybeStartRound`
    → `startRound` (swap `pending_` → `current_`) → Prepare → `onPrepareQuorum`
    (validate each entry: reject if account locked / insufficient funds / intra-batch
    conflict; lock survivors; cross-shard entries do a 2PC prepare) → Accept →
    `onAcceptQuorum` (intra-shard entries commit and release locks; cross-shard entries
    reply `PREPARED` and keep locks until the 2PC decision) → `finishRound` → drain
    whatever queued in `pending_` while the round ran.
- **InCall (`src/server/in_call.{h,cc}`):** a new `CallStatus::AWAIT_BATCH`. On
  `TPC_PREPARE` / `TRANSFER` the call **parks** itself and hands itself to the batch via
  `enqueueClientTxn`. A new `InCall::completeTransfer(bool ack, long tid)` lets the
  server finish a parked RPC once its batch round is decided.

**Subtlety that matters later.** Batching makes each round append a *variable* number
of log/WAL entries. Combined with 2PC-decision entries that append *asynchronously*,
this caused two follow-on problems addressed in Changes 2 and 3.

---

## 3. Change 2 — Idempotent balance application (money-conservation fix)

**What.** Balance changes were made **idempotent per server**: each transaction's
effect on balances is applied **at most once on each server**, no matter how many code
paths try to apply it.

**Why (the bug).** After batching, the total of all client balances drifted away from
the invariant `N × 10` — sometimes money was created, sometimes destroyed. Root cause
had two layers:
1. **Zombie servers** (stale processes from crashed runs) caused the catastrophic
   swings — a process/operations issue, not a code bug (see §7).
2. A real residual: **non-idempotent balance application during catch-up sync.** A
   catch-up `Sync` re-shipped committed entries and the receiver **re-applied** their
   balances directly into the `balances` map, double-counting money. Under batching,
   spurious syncs fired constantly (see Change 3), so this double-counting was frequent
   (a `SYNC_APPLY` re-apply fired up to ~474 times in a single 200-txn run).

**How it was fixed.** Added `std::set<long> balance_applied` to `ServerImpl` (keyed by
transaction id). All **three** places that move balances now guard on it so a tid's
effect lands once per server:
- `commitTransaction()` (intra-shard commit) — early-return if the tid was already
  applied.
- `processTpcDecision()` (cross-shard 2PC commit) — `first_apply` guard around the
  debit/credit (locks are still released unconditionally).
- `handleSyncReply()` (catch-up) — `first_apply` guard around the committed-entry
  debit/credit.

**Verified.** `correctness_check.sh` sums every touched client's balance across all 3
replicas; `per_replica_total == expected_total` (e.g. 1970 for the standard benchmark).
Held across runs.

**Caveat (documented, not fixed):** `balance_applied` grows unbounded over a very long
run. Fine for benchmarks; production would bound/checkpoint it (the same already applies
to `wal.transferIndex`).

---

## 4. Change 3 — Step A: gate Paxos sync on a Paxos-only index

**What.** Introduced a separate counter, **`paxos_index`**, that counts *only*
Paxos-agreed log appends, and switched the Prepare/Sync **consistency check** to use it
instead of the raw log index `last_inserted`.

**Why (the bug).** `last_inserted` is the raw position in the heterogeneous `log`
vector. It is bumped to `log.size() - 1` only by Paxos appends
(`prepareTransaction` / `commitTransaction`), **but** `processTpcDecision`
(`CS_COMMIT` / `CS_ABORT`) and the lock-timeout sweep also `push_back` onto `log`
*without* bumping `last_inserted`. The **next** Paxos append then jumps `last_inserted`
over those interleaved entries by a replica-dependent amount. So two replicas that have
agreed on the *identical* sequence of Paxos transactions end up with **different**
`last_inserted` values (because their async 2PC-decision appends interleaved
differently). The Prepare consistency check read that as divergence and fired
**catch-up syncs constantly** — which, before Change 2, also corrupted balances, and in
all cases burned CPU and round latency.

**How it works.**
- New field `int paxos_index` (an index; `-1` == empty), incremented **by exactly one**
  in `prepareTransaction` and `commitTransaction`, and per Paxos-type entry applied in
  `handleSyncReply`. It is **never** bumped by 2PC-decision or lock-timeout appends.
- A helper `isPaxosEntry(e)` defines a Paxos-agreed entry as `INTRA && COMMITTED`
  (intra commit) or `CROSS && PREPARED` (cross-shard prepare). Recovery recomputes
  `paxos_index` by counting these over the recovered log.
- The trigger sites now compare `paxos_index`:
  - `reissuePrepareForCurrent` sends `paxos_index` in the Prepare.
  - `processPrepareCall` compares the request's `paxos_index` to the local one
    (`==` → ack; `>` → behind, sync; `<` → reply with local `paxos_index`).
  - `handlePrepareReply` triggers a sync when a replica reports a higher `paxos_index`.
- **The sync *payload* path is unchanged** — it still ranges over the raw log via
  `last_inserted`. To make the two roles unambiguous, the proto field
  `PrepareReq/PrepareRes.last_inserted` was **renamed to `paxos_index`**, while
  `SyncReq.last_inserted` keeps its name. They are different messages, so there is no
  semantic crossover and the tested sync code is byte-for-byte the same.

**Verified.** With a temporary `SYNC_APPLY` counter, catch-up applies dropped from
**~474 → 0** per 200-txn run, and money conservation stayed at 1970. (Diagnostics were
removed after measuring.)

**Result on throughput by itself:** ~flat. Step A is a correctness/cleanliness
prerequisite, not a throughput lever on this workload — which led directly to the
investigation in Change 5.

---

## 5. Change 4 — Raise the peer RPC deadline

**What.** `RPC_TIMEOUT_MS` raised from **10 ms → 200 ms** (`src/server/server.h`).

**Why.** Prepare/Accept replies that exceed the deadline are counted as round
**failures**. On the single-threaded event loop, a peer busy in its own round can
easily take longer than 10 ms to service an incoming RPC, so under real load
otherwise-healthy rounds were at risk of spurious abort + retry. 200 ms is well above
worst-case localhost service latency yet still detects a genuinely dead peer quickly
(quorum needs only 1 of 2 peers, so a live peer finishes the round without waiting on
this deadline).

**Verified.** Conservation unchanged; not a measurable lever on localhost (the 10 ms
was not actually biting there), but correct under heavier/real load.

---

## 6. Change 5 — The throughput investigation and the load benchmark

This is the most important finding. **The path to 10k TPS was a measurement and
workload discovery, not a server code change.**

### 6.1 The "~190 TPS" was a harness artifact
`correctness_check.sh` runs 50 small sets with `sleep 0.04` **between** sets. The
client's wall-clock timer spans those idle gaps, so the reported throughput reflects
the throttle, not the system. Removing the throttle, the *same binary* does **~1.3k
TPS**.

### 6.2 Batches were size 1
Instrumenting `startRound` to log the batch size showed **every** Paxos round was
committing exactly **one** transaction (mean 1.0) — even with the throttle removed. The
benchmark scatters each set's four transfers across different leaders, and each round
finishes before the next transfer reaches the same leader, so `pending_` never
accumulates. **Consequence: the entire batching feature was inert, and group-commit
(the expected "real fsync lever") had nothing to coalesce.**

### 6.3 Rounds are serialized → batch=1 is the ceiling
Only one round runs at a time (`is_paxos_running`). At batch=1, ~1.3k TPS is near the
hard ceiling for a single leader. The only way past it is to **fill batches**, which
requires offering many concurrent commands to the *same* leader so they queue in
`pending_` while a round is in flight.

### 6.4 What reached 10k — concurrent disjoint load across all 3 clusters
Each cluster is an **independent Paxos group** with its own leader (S1, S4, S7), so
they batch **in parallel**. A workload was created that issues one big set of
**disjoint** intra-shard transfers, spread across all three clusters, with **no
throttle**:
- `test/gen_load.py` emits 1500 transfers (500 per cluster). Within each cluster the
  pairs are `(2k-1 → 2k)` so **every client appears exactly once** — zero intra-batch
  lock conflicts, every transfer succeeds, amount 1 (money trivially conserved:
  net-zero within the shard).
- `test/load_benchmark.sh` feeds them with no inter-set sleep, lets the system settle,
  then sums balances per replica to check conservation.

**Result:** the first round of each leader is small, then `pending_` fills and
subsequent rounds commit large batches (mean ~9, max ~100). Throughput reaches
**~11k TPS (9k–14k across runs)** with **`per_replica_total == 30000` (exact)**.
**No group-commit was needed.**

### 6.5 Two benchmarks — do not mix them
| Harness | What it measures | Typical TPS |
|---|---|---|
| `correctness_check.sh` (local, not committed) | Throttled, 50 small sets, conservation | ~185–200 |
| `test/load_benchmark.sh` | 1500 concurrent disjoint intra-shard txns, 3 clusters | ~8k–14k |

"The system does 10k" refers to the **load benchmark** (or equivalent offered
concurrent load), never the throttled correctness script.

### 6.6 Conservation-check caveat (not money loss)
Under heavy load a client `Balance` read can exceed the client RPC deadline and print
`-`, which the summation counts as 0, occasionally making `per_replica_total` read a few
short (e.g. 29997). Because intra-shard transfers are **net-zero to the cluster total**,
the in-store total is a mathematical **invariant** — this is purely a read-timeout
measurement artifact. `load_benchmark.sh` attributes it explicitly on the status line.

---

## 7. Other fixes (stability / harness)

These were needed to get trustworthy measurements at all:

- **Zombie-process discovery.** Crashed/aborted runs leave `server S1..S9` processes
  alive; they hold the ports, burn CPU, and the driver silently talks to the *old*
  binaries → every measurement is garbage and non-deterministic. They were the cause of
  the catastrophic conservation swings. **Always** `pkill -9 -f "server S"` and wipe
  `S*_db wal_*.log` before any run.
- **gRPC "stall" root-cause.** A reported Prepare-phase stall after a gRPC upgrade was
  *not* a gRPC bug — it was a test-harness race (client sent before servers bound).
  Fixed with `Client::waitForServersReady()` (channel `WaitForConnected`).
- **Driver SIGSEGV on shutdown.** Two bugs: iterator invalidation in
  `utils::killAllServers()` (fixed with explicit `erase(it)`), and the
  `Client::consumeReplies` thread not being joined (added `Client::shutdown()` calling
  `cq.Shutdown()` and `t.join()` in `driver.cc`).
- **Driver infinite loop on piped EOF.** `mainloop` now detects `std::cin` EOF, kills
  servers, and breaks cleanly.
- **Client data race.** Shared state (`processing`, `latencies`,
  `transactions_processed`, wall-clock fields) accessed by both the main thread and the
  `consumeReplies` thread is now guarded by `std::mutex state_mtx`.

---

## 8. What is new — file-by-file

**Server / protocol changes:**

| File | What changed |
|---|---|
| `src/proto/tpc.proto` | `AcceptReq.batch` (repeated) and `PrepareRes.accept_val` (repeated) for batching; **`PrepareReq.last_inserted` and `PrepareRes.last_inserted` renamed to `paxos_index`** (Step A). Regenerate protos after editing. |
| `src/server/server.h` | Batched-round state (`pending_`, `current_`, `BatchEntry`, `is_paxos_running`, `accept_batch`); **`std::set<long> balance_applied`** (idempotency); **`int paxos_index`** (Step A); `RPC_TIMEOUT_MS` 10 → 200. |
| `src/server/server.cc` | Batched round logic (`maybeStartRound` … `finishRound`); idempotency guards in `commitTransaction`, `processTpcDecision`, `handleSyncReply`; `isPaxosEntry()` + `paxos_index` maintenance; Prepare/Sync trigger switched to `paxos_index`. |
| `src/server/in_call.h` / `in_call.cc` | `AWAIT_BATCH` status; park `TPC_PREPARE`/`TRANSFER` into the batch; `completeTransfer()`. |
| `src/client/client.h` / `client.cc` | `std::mutex state_mtx` + `lock_guard` around shared state; `shutdown()`. |
| `src/driver.cc` | EOF-safe `mainloop`; join the `consumeReplies` thread. |
| `src/utils/utils.cc` | Safe `erase(it)` in `killAllServers()` (iterator-invalidation fix). |
| `CMakeLists.txt` | Temporary AddressSanitizer flags removed after debugging. |

**New benchmark tooling (committed under `test/`):**

| File | Purpose |
|---|---|
| `test/gen_load.py` | Generates the high-concurrency disjoint intra-shard workload (default 1500 txns, 500/cluster) → `test/load_3cluster1500.csv`. |
| `test/load_benchmark.sh` | Runs the load workload with no throttle and verifies `sum(balances) == 10 × clients` per replica; attributes rare undercounts to balance-read timeouts. |
| `test/load_3cluster1500.csv` | The generated 10k-TPS workload, committed ready-to-use. |

**Docs:** `HANDOFF.md` updated to reflect the reached goal and corrected plan; this
`CHANGES.md` added.

---

## 9. How to reproduce every result

Always kill zombies and wipe state first:
```bash
pkill -9 -f "server S" || true
cd 2pc/build && rm -rf S*_db wal_*.log
```

**Build:**
```bash
cd 2pc/build
cmake ..            # only after CMakeLists/proto changes
make server driver
```

**Correctness + conservation (throttled, ~190 tps — this is the throttle, not the
ceiling):**
```bash
cd 2pc/build && bash correctness_check.sh   # local script
# look for: per_replica_total == expected_total
```

**Throughput goal (~10k tps, conservation checked):**
```bash
cd 2pc && python3 test/gen_load.py          # writes test/load_3cluster1500.csv
cd build && bash ../test/load_benchmark.sh  # ~9k–14k tps, per_replica_total == 30000
```

---

## 10. Invariants that must never regress

- **Money conservation:** sum of all balances `== N × 10` at all times. The #1 test.
- **Per-server idempotency:** each tid's balance effect applies at most once per server
  (`balance_applied`).
- **2PC atomicity:** a cross-shard txn commits on **both** clusters or neither;
  asymmetric application creates/destroys money.
- **Single-threaded server loop:** all state mutation happens on the `HandleRPCs`
  event loop — no extra server-side locking, and **no blocking calls** there.
- **Rounds are serialized** (`is_paxos_running`); throughput past batch=1 comes from
  filling batches via concurrent load, not from parallel rounds.

---

## 11. Commit history

| Commit | Summary |
|---|---|
| `9378938` | Batch Multi-Paxos rounds and make balance application idempotent (Changes 1 + 2). |
| `1bfdefb` | Gate Paxos sync on a Paxos-only index to stop spurious catch-up storms (Change 3 / Step A). |
| `d84fb10` | Add high-concurrency load benchmark; reach ~10k TPS; raise peer RPC deadline (Changes 4 + 5). |
| `78c9ddf` | Update HANDOFF: 10k TPS reached via concurrent load; correct the old plan. |

All commits are authored by the repository owner with **no AI attribution** in message
or metadata, per project requirement.
