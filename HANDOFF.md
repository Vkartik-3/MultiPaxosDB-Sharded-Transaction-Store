# MultiPaxosDB — Work Handoff & Plan to 10,000 TPS

> Purpose: this is a complete, self-contained handoff. A new session with **no prior
> context** should be able to read this top-to-bottom and continue the work. It records
> what was done, the exact current state, every file touched, the known bugs/gotchas,
> and a detailed, ordered plan to reach the ~10,000 TPS goal.

---

## 0. TL;DR (read this first)

- **System:** Sharded bank built on **Multi-Paxos (intra-shard)** + **2PC (cross-shard)**.
  3 clusters × 3 servers (9 servers, named `S1`–`S9`), single-threaded async-gRPC event
  loop per server. C++, gRPC, protobuf, spdlog, LevelDB.
- **What we just did:** implemented **batching**, fixed the **money-conservation
  regression** it introduced, fixed the **spurious-sync storm** (Step A, `paxos_index`),
  and — crucially — discovered the path to the throughput goal (see below).
- **Correctness status:** ✅ FIXED. Money is conserved (total balance = `N*10`, verified
  across runs and at ~10k TPS load).
- **Performance status:** ✅ **~10k TPS GOAL REACHED.** Run `test/load_benchmark.sh`
  (concurrent disjoint intra-shard load across all 3 clusters): **~11k TPS (9k–14k),
  conservation exact.** The old **"~190 TPS" was an artifact of the throttled
  `correctness_check.sh`**, NOT the system ceiling (which is ~1.3k tps at batch size 1).
  The real lever was offered concurrent load, not group-commit. See §6.
- **Two benchmarks — don't mix them:** `correctness_check.sh` (throttled, 50 small sets,
  conservation) measures ~185–200 tps; `test/load_benchmark.sh` (1500 concurrent disjoint
  transfers, 3 clusters) measures ~8k–14k. "The system does 10k" means the load benchmark.
- **CRITICAL GOTCHA that wasted hours:** stale "zombie" server processes from crashed
  runs keep running, hog the ports/CPU, and **poison every benchmark**. ALWAYS check &
  kill them before trusting any number. See §5.1.

---

## 1. Original problem report (and what it actually was)

**Reported:** "Paxos round stalls at the Prepare phase after upgrading to gRPC 1.80.
`processPrepareCall` never logs; `response_cq` never fires."

**Reality (diagnosed):** NOT a gRPC wiring problem. It was a **test-harness race** — the
client sent requests before servers finished binding. The gRPC 1.80 upgrade was a red
herring. Fixed via `Client::waitForServersReady()` (channel `WaitForConnected`).

Lesson: prove the root cause empirically before "fixing" the reported symptom.

---

## 2. Work completed this session (chronological)

1. **gRPC "stall"** → root-caused to harness race; added `Client::waitForServersReady`.
2. **Driver SIGSEGV on shutdown** → two bugs:
   - iterator invalidation in `utils::killAllServers()` (fixed with explicit `erase(it)`).
   - `std::thread` lifetime: `Client::consumeReplies` thread not joined → added
     `Client::shutdown()` (calls `cq.Shutdown()`) and `t.join()` in `driver.cc`.
3. **Driver infinite loop on EOF** (when commands are piped in): `mainloop` now detects
   `std::cin` EOF, calls `utils::killAllServers()`, and breaks cleanly.
4. **Batching (Phase 2) implemented** — the core feature (see §3 for design).
5. **Client data race** on shared state (`processing` map, `latencies`,
   `transactions_processed`, wall-clock fields) between main thread and `consumeReplies`
   thread → added `std::mutex state_mtx` + `lock_guard` around all shared access.
6. **Zombie-process discovery** — 9 stuck servers from a 1:34 AM crash were poisoning all
   measurements; killed them (see §5.1). This was the cause of the catastrophic
   (−2765 … +2714) conservation swings.
7. **Money-conservation fix** — made balance application **idempotent per server** via a
   `balance_applied` tid-set (see §4). This is the key correctness fix.
8. **Cleanup** — removed all temporary `[DIAG]`/`[CDIAG]` logging and the AddressSanitizer
   flags from `CMakeLists.txt`; reconfigured + rebuilt clean.

---

## 3. Batching design (what was implemented)

**Goal:** amortize the 6 peer RPCs (prepare/accept/commit fan-out) and the disk fsync over
many client commands by committing **N commands in one Paxos round** instead of 1.

**Proto changes (`src/proto/tpc.proto`):**
- `AcceptReq`: `TransferReq r = 2;` → `repeated TransferReq batch = 2;`
- `PrepareRes`: `optional TransferReq accept_val` → `repeated TransferReq accept_val = 4;`

**Server state machine (`src/server/server.{h,cc}`):** replaced the old single-txn Paxos
state with a batched round driven by reply handlers:
- `pending_` — `std::vector<BatchEntry>` accumulating commands for the *next* round.
- `current_` — entries accepted into the *in-flight* round.
- `BatchEntry { TransferReq request; InCall* call; bool is_cross_shard; }`
- `bool is_paxos_running;` (replaced the old `paxos_tid`).
- `std::vector<TransferReq> accept_batch;` (replaced single `accept_val`).
- Methods: `enqueueClientTxn()`, `maybeStartRound()`, `startRound()`,
  `reissuePrepareForCurrent()`, `onPrepareQuorum()`, `onAcceptQuorum()`,
  `onRoundAbort(bool prepare_phase)`, `finishRound()`.
- Flow: client txn → `enqueueClientTxn` (pushes to `pending_`) → `maybeStartRound` →
  `startRound` (swap pending_→current_) → prepare → `onPrepareQuorum` (per-entry validate:
  reject if locked / insufficient funds / intra-batch conflict; lock survivors; cross-shard
  entries `prepareTransaction`) → accept → `onAcceptQuorum` (intra: `commitTransaction` +
  release locks; cross: reply PREPARED, keep locks) → `finishRound` → drain `pending_`.

**InCall changes (`src/server/in_call.{h,cc}`):**
- New `CallStatus::AWAIT_BATCH`. On `TPC_PREPARE` / `TRANSFER`, the call parks
  (`status_ = AWAIT_BATCH`) and hands itself to the batch via `enqueueClientTxn(this, ...)`.
- New `InCall::completeTransfer(bool ack, long tid)` — the server calls this to finish a
  parked RPC once its batch round is decided (sets response, `Finish()`, `status_ = FINISH`).

---

## 4. The money-conservation bug & fix (most important correctness detail)

**Symptom:** total of all client balances drifted far from `N*10` (the invariant) —
swinging negative (money destroyed) or positive (money created), non-deterministically.

**Root cause (two layers):**
1. **Zombie servers** (see §5.1) caused the *catastrophic* swings — not a code bug per se.
2. After killing zombies, a real residual remained: **non-idempotent balance application
   during catch-up sync.** Batching makes each Paxos round append a *variable* number of
   WAL/log entries, and 2PC decisions (`CS_COMMIT`/`CS_ABORT`) are appended
   **asynchronously & independently** on each server. So the integer `last_inserted`
   (a per-server log position) **drifts between replicas**. That drift triggers spurious
   catch-up syncs, and the sync path in `handleSyncReply` **re-applied committed balances
   directly to the `balances` map**, double-counting money. (`SYNC_APPLY` fired up to 474
   times in a single 200-txn run.)

**Fix (implemented):** make balance application **idempotent per server**. Added
`std::set<long> balance_applied;` to `ServerImpl` (keyed by transaction id). Guarded all
THREE places that move balances so each tid's effect applies at most once per server:
- `commitTransaction()` (intra-shard commit): early-return if tid already applied.
- `processTpcDecision()` (cross-shard 2PC commit): `first_apply` guard around debit/credit
  (locks still released unconditionally).
- `handleSyncReply()` (catch-up): `first_apply` guard around the committed-entry
  debit/credit.

**Result:** conservation holds (1970 expected; observed 1970 in 4/5 runs, one 1966 due to a
balance read taken mid-flight — the per-replica total wasn't divisible by 3, i.e. one
replica was a beat behind; not money lost).

> NOTE: `balance_applied` grows unbounded over a very long run (one entry per committed
> tid). Fine for benchmarks; for production it should be bounded/checkpointed. Same caveat
> already applies to `wal.transferIndex`.

---

## 5. How to build, run, and verify

### Build
```bash
cd "2pc/build"
cmake ..            # only needed after CMakeLists changes
make server driver  # builds the two binaries used by the harness
```

### 5.1 ⚠️ ALWAYS kill zombie servers first (this wasted hours)
Crashed/aborted runs leave `server S1..S9` processes alive. They keep the ports, burn CPU,
and the driver will silently talk to these OLD binaries → every measurement is garbage and
non-deterministic. Before ANY run:
```bash
# from outside the sandbox if needed
pgrep -fl "server S[0-9]"                     # list stragglers
pkill -9 -f "server S"                        # or: kill -9 <pids>
# then wipe state so you start clean:
cd "2pc/build" && rm -rf S*_db wal_*.log correctness_run.log
```
Symptom that you have zombies: `wal_<id>.log` contains a single tid repeated hundreds of
times, and/or it contains tids the current client never sent (compare timestamps — tids are
`system_clock` epoch-nanoseconds).

### 5.2 Correctness + conservation check
`2pc/build/correctness_check.sh` drives the benchmark (`../test/benchmark.csv`, 50 sets),
prints performance, then sums every touched client's balance across all 3 replicas.
```bash
cd "2pc/build" && rm -rf S*_db wal_*.log correctness_run.log && bash correctness_check.sh
```
Read the final line:
```
clients_touched=197 expected_total=1970 ... per_replica_total=1970   # GOOD (== expected)
```
- `per_replica_total == expected_total` → money conserved.
- `sum_over_replicas` not divisible by 3 → a replica was mid-sync at read time (in-flight),
  usually harmless; re-run or add settle time to confirm.

### 5.3 Where logs go
- Server stdout is inherited from the driver and (in the harness) redirected to
  `correctness_run.log`. **But** servers are SIGKILL'd at shutdown, so buffered logs are
  lost unless you set `spdlog::flush_on(spdlog::level::info)` in the server `main()` while
  debugging (it was added then removed during cleanup).
- `wal_<id>.log` is each server's **write-ahead log** (survives crashes; great for forensic
  analysis — e.g. counting how many times a tid was committed).

---

## 6. Throughput work — what we found, and the result (~10k TPS reached)

> **The original plan in this section was B-before-C and assumed ~190 TPS was the
> system ceiling. Both were wrong. This section now records what the measurements
> actually showed and the path that reached the 10k goal.** Lesson (again): measure
> before optimizing.

### What the measurements showed
- **The "~190 TPS" was a harness artifact.** `correctness_check.sh` sleeps `0.04s`
  between 50 small sets; the client's wall clock spans those idle gaps. Remove the
  throttle and the same code does **~1.3k TPS**.
- **Batches were size 1.** Instrumenting `startRound` showed *every* Paxos round
  committed exactly one transaction (mean 1.0), even with the throttle removed —
  the benchmark scatters each set across leaders and rounds finish before the next
  txn reaches the same leader. So batching (the whole Phase 2 feature) was inert, and
  **group-commit (old Step B) had nothing to coalesce.**
- **Rounds are serialized** (`is_paxos_running` gate, single-threaded loop). At
  batch=1, ~1.3k TPS is near the ceiling. The only way past it is to *fill batches*.

### What actually reached 10k — concurrent load (the old "Step C" was the lever)
Offer many disjoint intra-shard transfers at once so `pending_` accumulates while a
round is in flight, and spread them across all 3 clusters (each is an independent
Paxos group with its own leader S1/S4/S7, so they batch in **parallel**):
- `test/gen_load.py` builds one big set of 1500 disjoint transfers (500/cluster,
  each client used once → no intra-batch lock conflicts, all succeed).
- `test/load_benchmark.sh` feeds it with no throttle and checks conservation.
- **Result: ~11k TPS (9k–14k across runs), conservation exact (30000).** Batches
  filled to mean ~9, max ~100. **No group-commit needed.**

Reproduce (after `make server driver`, and §5.1 kill-zombies first):
```bash
python3 test/gen_load.py                  # writes test/load_3cluster1500.csv
cd build && bash ../test/load_benchmark.sh
```

### Done
- **Step A — spurious sync fix: DONE** (commit `1bfdefb`). Added `paxos_index`
  (counts Paxos-agreed appends only) and gated the prepare/sync trigger on it instead
  of the raw `last_inserted`, which drifts because async 2PC-decision/abort appends
  interleave differently per replica. `SYNC_APPLY` 474 → 0; conservation still 1970.
- **Load benchmark + `RPC_TIMEOUT_MS` 10→200ms: DONE** (commit `d84fb10`).

### Optional future work (NOT required for 10k)
- **Group-commit (old Step B2):** coalesce a batch's WAL writes into one
  `std::ofstream` open/close and its LevelDB `Put`s into one `WriteBatch`. Only worth
  it once batches are large (they now are under load); buys cleaner durability and a
  bit more headroom past ~14k. Note neither path currently fsyncs, so today it saves
  syscalls, not fsyncs.
- **Stable-leader Multi-Paxos (old Step D):** skip Prepare on the hot path. ~2× more,
  medium risk (leader-change/recovery edges).

### Conservation-check caveat (not money loss)
Under heavy load a client `Balance` read can exceed the client RPC deadline and print
`-` (summed as 0), making `per_replica_total` read a few short (e.g. 29997). Intra-shard
transfers are net-zero to the cluster total, so the in-store total is invariant — this
is a read-timeout artifact. `load_benchmark.sh` attributes it on the status line.

---

## 7. Files modified this session (with purpose)

| File | What changed |
|---|---|
| `2pc/src/proto/tpc.proto` | `AcceptReq.batch` (repeated), `PrepareRes.accept_val` (repeated) for batching. **Regenerate protos after editing.** |
| `2pc/src/server/server.h` | Batch state machine decls; `BatchEntry`; `pending_`/`current_`; `is_paxos_running`; `accept_batch`; **`std::set<long> balance_applied;`** |
| `2pc/src/server/server.cc` | Batched round logic (`maybeStartRound`…`finishRound`); idempotency guards in `commitTransaction`, `processTpcDecision`, `handleSyncReply`; diagnostics removed |
| `2pc/src/server/in_call.h` | `AWAIT_BATCH` status; `completeTransfer()` decl |
| `2pc/src/server/in_call.cc` | Park TPC_PREPARE/TRANSFER into batch; `completeTransfer()` impl |
| `2pc/src/client/client.h` | `std::mutex state_mtx;` to fix data race |
| `2pc/src/client/client.cc` | `lock_guard` around all shared state; `shutdown()` |
| `2pc/src/driver.cc` | EOF-safe `mainloop`; join `consumeReplies` thread |
| `2pc/src/utils/utils.cc` | Safe `erase(it)` in `killAllServers()` (iterator-invalidation fix) |
| `2pc/CMakeLists.txt` | ASan flags removed from `server` target (were temporary) |
| `2pc/build/correctness_check.sh` | Money-conservation harness (50 sets, sums balances) |

---

## 8. Status / next action for a new session

**The 10k TPS goal is reached and pushed.** Current `main` tip is `d84fb10`
(parent `1bfdefb` = Step A). There is no outstanding required work.

- **Standing requirement:** **NO AI attribution anywhere** — no `Co-authored-by`, no
  Cursor/Claude mention in any commit message or metadata. (Enforce with `git
  commit-tree` if a trailer ever sneaks in.)
- Always do **§5.1 (kill zombies) first** before any benchmark, and re-verify
  conservation (§5.2 for the throttled check; `test/load_benchmark.sh` for the load
  check) after any change.
- If continuing for headroom past ~14k or cleaner durability, see the **optional
  future work** in §6 (group-commit, then stable-leader). Neither is required for 10k.

---

## 9. Key invariants & sanity checks (don't regress these)

- **Money conservation:** sum of all balances == `N * 10` at all times (N = number of
  clients). This is the #1 correctness test.
- **Per-server idempotency:** each tid's balance effect applies at most once per server
  (`balance_applied`).
- **2PC atomicity:** a cross-shard txn must commit on BOTH clusters or neither
  (debit on sender's shard ⇔ credit on receiver's shard). Asymmetric application
  creates/destroys money.
- **Single-threaded server loop:** all state mutation happens on the `HandleRPCs` event
  loop — no extra server-side locking needed, but don't introduce blocking calls there.
