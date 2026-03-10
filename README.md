# MultiPaxosDB — Sharded Transaction Store

## Abstract

A fault-tolerant distributed transaction processing system supporting a banking application. Servers are partitioned into multiple clusters, each maintaining a replicated data shard. The system handles two transaction types: **intra-shard** (consensus via Multi-Paxos) and **cross-shard** (coordination via Two-Phase Commit). Fault tolerance is achieved through replication within each cluster under a fail-stop failure model.

---

## Table of Contents

- [Architecture](#architecture)
- [Protocols](#protocols)
  - [Intra-Shard: Multi-Paxos](#intra-shard-transactions-multi-paxos)
  - [Cross-Shard: Two-Phase Commit](#cross-shard-transactions-two-phase-commit)
- [System Design](#system-design)
  - [Storage Layer](#storage-layer)
  - [Write-Ahead Log (WAL)](#write-ahead-log-wal)
  - [Concurrency & Locking](#concurrency--locking)
  - [Networking](#networking)
  - [Fault Tolerance](#fault-tolerance)
- [Implementation Details](#implementation-details)
  - [WAL Recovery on Startup](#1-wal-recovery-on-startup)
  - [Ballot Number Persistence](#2-ballot-number-persistence)
  - [2PC Lock Timeout](#3-2pc-lock-timeout)
  - [Configuration File Support](#4-configuration-file-support)
  - [Benchmark Instrumentation](#5-benchmark-instrumentation)
- [Benchmark Results](#benchmark-results)
- [Build & Run](#build--run)
- [Project Structure](#project-structure)

---

## Architecture

Servers are organized into clusters where each cluster manages a distinct shard of the data. The client is aware of the shard-to-cluster mapping and routes requests accordingly.

<p align="center">
  <img src="https://github.com/user-attachments/assets/6ec30b18-769a-4951-84c7-435213b7218c">
  <br>
  <em>Figure 1: System Architecture — 9 servers, 3 clusters, 3 shards</em>
</p>

The data is divided into three shards D1, D2, D3. Nine servers (S1–S9) are organized into three clusters C1, C2, C3. Each shard Di is fully replicated across all servers in cluster Ci. The system tolerates at most one server failure per cluster (majority quorum = 2-of-3).

<p align="center">
  <img src="https://github.com/user-attachments/assets/043e8b56-61d4-42cb-b208-5fd045ed809e">
  <br>
  <em>Figure 2: Data partitioning — 6 data items split across 3 shards</em>
</p>

---

## Protocols

### Intra-Shard Transactions: Multi-Paxos

For intra-shard transactions (both sender and receiver in the same cluster), the system runs a full Paxos round per transaction — no persistent log is assumed on startup (consensus is required for each transaction individually).

**Protocol flow:**

1. **Client → Leader**: Client sends `TransferReq(sender, receiver, amount)` to a randomly selected server in the relevant cluster.
2. **Proposer checks preconditions**: No locks held on sender/receiver; sender balance ≥ amount.
3. **Prepare phase**: Proposer sends `Prepare(ballot_num)` to all peers. Each peer responds with `Promise(ballot_num, accepted_ballot, accepted_val)` and synchronizes state if needed (the server with the stale state pulls the missing committed entry from the proposer).
4. **Accept phase**: On receiving a quorum of promises, proposer sends `Accept(ballot_num, txn)` to peers.
5. **Commit phase**: On receiving a quorum of accepts, proposer commits, applies balance changes to LevelDB, writes an `IN_COMMIT` entry to the WAL, and replies to the client.

**Ballot number**: Monotonically increasing integer, persisted to LevelDB after every increment so it survives crashes.

### Cross-Shard Transactions: Two-Phase Commit

The client acts as the 2PC coordinator. Each involved cluster runs Paxos internally to reach consensus on the prepared/aborted state before responding.

<p align="center">
  <img src="https://github.com/user-attachments/assets/3b997256-ee5a-4d2f-bd87-85d0740ccb6e">
  <br>
  <em>Figure 3: Cross-shard transaction flow between C1 and C2</em>
</p>

**Phase 1 — Prepare:**

1. Client sends `TpcPrepare(tid, sender, receiver, amount)` to a server in each involved cluster simultaneously.
2. Each cluster runs internal Paxos. The sender-side cluster additionally checks that `balance(sender) ≥ amount`.
3. Locks are acquired on the accessed data items using two-phase locking. If a lock is already held, the cluster votes **abort**.
4. Cluster leaders write a `CS_PREPARE` entry to the WAL and respond `PREPARED` or `ABORT` to the client.

**Phase 2 — Commit or Abort:**

- If both clusters respond `PREPARED`: client broadcasts `TpcCommit(tid)` to all servers in both clusters. Each server applies the balance change, writes `CS_COMMIT` to WAL, releases locks, and acks.
- If any cluster responds `ABORT` or the coordinator times out: client broadcasts `TpcAbort(tid)` to all servers in both clusters. Each server rolls back using WAL, writes `CS_ABORT`, releases locks, and acks.

**Transaction ID**: The TID is the `system_clock` epoch in nanoseconds at request submission — globally unique and used to correlate prepare/commit/abort messages.

---

## System Design

### Storage Layer

**LevelDB** is used for durable key-value storage of account balances. Each server in a cluster maintains its own LevelDB instance (`S{id}_db/`).

- Account balances are stored as `"client_id" → "balance"` string pairs.
- The ballot number is stored as `"__ballot_num__" → "N"` for crash recovery.
- On startup, balances are loaded from LevelDB (initialized to 10 if not found (initial balance per account)), then WAL metadata is replayed on top.

### Write-Ahead Log (WAL)

Each server maintains a plain-text append-only WAL (`S{id}.wal`) for 2PC durability. Four entry types:

| Entry Type  | When Written                                     | Format                                      |
|-------------|--------------------------------------------------|---------------------------------------------|
| `IN_COMMIT` | Intra-shard commit                               | `IN_COMMIT tid ballot sender receiver amt`  |
| `CS_PREPARE`| Cross-shard prepare (locks acquired, Paxos done) | `CS_PREPARE tid ballot sender receiver amt` |
| `CS_COMMIT` | Cross-shard commit decision received             | `CS_COMMIT tid ballot sender receiver amt`  |
| `CS_ABORT`  | Cross-shard abort decision received              | `CS_ABORT tid ballot sender receiver amt`   |

**Recovery** (`WAL::recover()`):
- Reads WAL line by line on startup.
- Rebuilds in-memory metadata: `log[]` (committed entries), `transferIndex` (pending prepared tids), `last_inserted_ballot`, `ballot_num`.
- Does **not** re-apply balances — LevelDB is the source of truth for balances.
- Skips malformed lines; tracks max ballot seen across all entries.

### Concurrency & Locking

The server runs a **single-threaded event loop** (`HandleRPCs()`). All business logic executes on one thread, polling two gRPC completion queues sequentially:

- Incoming RPC queue (client requests, peer responses received as callbacks)
- Outgoing RPC completion queue

This design eliminates data races entirely — no mutexes needed for in-memory state (`balances`, `locks`, `processing`, `log`, WAL index). The only shared concurrency concern is gRPC's internal thread pool, which is handled by gRPC itself.

**Two-phase locking**: Locks are acquired at prepare time and released only after commit or abort. A pending-prepare map tracks lock holders per transaction ID.

### Networking

All communication uses **gRPC with async completion queues** (`ClientAsyncResponseReader`). The client submits RPCs non-blocking and processes replies via `consumeReplies()` on a dedicated thread. All peer-to-peer Paxos messages use unary RPCs.

**RPC timeout**: Client-side deadline set to 10ms per RPC call. Unreachable servers are tracked and skipped.

**Protocol Buffers 3** defines all message types: `TransferReq`, `TransferRes`, `TpcTid`, `Ballot`, `Empty`, and service `TpcServer`.

### Fault Tolerance

- **Majority quorum**: 2-of-3 servers must respond for Paxos to proceed. One server failure per cluster is tolerated.
- **State synchronization**: During the Prepare phase, servers compare `last_inserted_ballot`. The server with the stale state pulls the missing committed entry from the proposer before sending its Promise.
- **Crash recovery**: On restart, a server reloads ballot number and balances from LevelDB, then replays WAL to restore pending 2PC state.
- **2PC lock timeout**: Locks held by unresolved prepared transactions are automatically released after 5 seconds (server-side sweep in the event loop). This prevents permanent deadlock when the coordinator crashes after Phase 1.
- **Disconnected servers**: The client tracks disconnected server addresses and skips them when routing requests.

---

## Implementation Details

### 1. WAL Recovery on Startup

**Problem**: On server restart, the in-memory state was always reinitialized to defaults (balances = 10, empty log). This meant prepared cross-shard transactions were silently forgotten, leaving locks permanently held on surviving replicas and making it impossible for Paxos to resume with the correct ballot.

**Fix**: `WAL::recover()` was implemented to:
- Parse all WAL entries on startup and classify each as `IN_COMMIT`, `CS_PREPARE`, `CS_COMMIT`, or `CS_ABORT`.
- Rebuild `log[]` with all committed intra-shard entries.
- Rebuild `transferIndex` with still-pending cross-shard prepares (entries with `CS_PREPARE` but no matching `CS_COMMIT`/`CS_ABORT`).
- Track the highest ballot number seen across all WAL entries.
- Restore `last_inserted_ballot` from the highest committed log entry.

Balances are intentionally **not** re-applied from WAL — LevelDB already has the durable committed state. The WAL is only used to rebuild metadata (pending locks, log sequence, ballot state).

### 2. Ballot Number Persistence

**Problem**: The ballot number was kept only in memory. After a crash, the server restarted with ballot 0, which could allow a restarted server to accept stale messages from a previous epoch or collide with in-progress proposals.

**Fix**: `persistBallotNum()` writes `ballot_num` to LevelDB (`"__ballot_num__" → N`) after every mutation:
- After increment in the proposer path (`++ballot_num` before Prepare)
- After updating to a higher ballot from a Promise response (acceptor sees higher ballot)
- After backing off on leader rejection (`ballot_num = response.latest_ballot_num()`)
- After syncing state from a peer during recovery (`ballot_num = response.last_inserted_ballot().num()`)

On startup, the persisted value is loaded before WAL replay, ensuring the recovered ballot is at least as high as the one from the last WAL entry.

### 3. 2PC Lock Timeout

**Problem**: If the client (coordinator) crashed after Phase 1 but before Phase 2, participating servers held their locks indefinitely. No other transaction involving those data items could proceed.

**Fix**: A `prepareTimestamps` map (`tid → steady_clock::time_point`) is maintained in the server. Every time a server records a `CS_PREPARE`, it stores the current time. The event loop (`HandleRPCs`) sweeps this map on every iteration:

```cpp
static constexpr int TPC_LOCK_TIMEOUT_MS = 5000;

for (auto it = prepareTimestamps.begin(); it != prepareTimestamps.end(); ) {
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - it->second).count();
    if (elapsed > TPC_LOCK_TIMEOUT_MS) {
        // release lock, write CS_ABORT to WAL, erase from processing
        it = prepareTimestamps.erase(it);
    } else {
        ++it;
    }
}
```

On a real commit or abort decision (`processTpcDecision`), the entry is removed from `prepareTimestamps` before the sweep can trigger.

### 4. Configuration File Support

**Problem**: Server addresses and IDs were hardcoded. Testing different cluster topologies or deploying to non-default addresses required a recompile.

**Fix**: An optional fourth argument `[config_filepath]` was added to the driver. The config file format is:

```
server_name  host:port
S1           localhost:50051
S2           localhost:50052
...
```

`utils::loadConfig(path)` parses the file, clears the existing address/ID maps, and replaces them with the loaded values. If the file is not found, it returns `false` and the defaults are used. `setupApplicationState` now clears maps before filling to prevent stale entries.

### 5. Benchmark Instrumentation

**Problem**: No latency or throughput measurements existed. Performance was invisible.

**Fix**: Per-transaction latency is measured end-to-end from request submission to reply receipt:

- **Intra-shard**: The TID (transaction ID) is set to `system_clock::now()` in nanoseconds at submission. On reply, latency = `system_clock::now() - tid`. Both endpoints use `system_clock` so clock domains match.
- **Cross-shard**: `steady_clock::now()` is stored in the `TpcPrepareRes` at submission. On final commit/abort ack, latency = `steady_clock::now() - start_ns`. `steady_clock` is used for intervals to avoid wall-clock jumps.

Wall-clock throughput = `transactions_processed / (wall_end - wall_start)` in seconds, measured from first transaction submission to last reply.

`printPerformance()` outputs:
- Total transactions, wall time, throughput (TPS)
- Overall p50 and p99 latency
- Intra-shard mean, p50, p99
- Cross-shard mean, p50, p99

---

## Benchmark Results

Benchmarks run on a single machine (macOS, Apple Silicon), 9 local server processes, 3 clusters × 3 servers each. Mix of intra-shard and cross-shard transactions from the test CSV.

| Run | Txns | Wall (s) | TPS   | p50 (ms) | p99 (ms) | Intra Mean (ms) | Intra p99 (ms) | Cross p50 (ms) | Cross p99 (ms) |
|-----|------|----------|-------|----------|----------|-----------------|----------------|----------------|----------------|
| 1   | 102  | 2.86     | 35.65 | 98.08    | 483.63   | 78.23           | 483.63         | 146.91         | 525.61         |
| 2   | 134  | 4.06     | 32.97 | 98.44    | 600.32   | 84.17           | 601.89         | 127.08         | 600.32         |
| 3   | 124  | 3.18     | 38.96 | 160.61   | 946.43   | 108.40          | 971.28         | 247.01         | 946.43         |
| 4   | 138  | 3.53     | 39.10 | 158.23   | 901.99   | 143.62          | 998.12         | 160.74         | 597.20         |
| 5   | 128  | 3.11     | 41.14 | 215.45   | 1250.12  | 132.77          | 1257.04        | 294.16         | 1250.12        |

**Summary (averages across 5 runs):**
- Throughput: ~37.6 TPS
- p50 latency: ~146 ms
- p99 latency: ~836 ms
- Cross-shard transactions have roughly 1.5–2× higher latency than intra-shard due to two sequential Paxos rounds plus the coordinator round-trip.

The p99 variance across runs reflects contention on hot accounts and lock serialization under the 2PL protocol.

---

## Build & Run

### Prerequisites

- macOS or Linux
- CMake ≥ 3.20
- gRPC ≥ 1.78 with matching Protobuf (protobuf@33 / abseil 20260107.x)
- spdlog

**On macOS with Homebrew:**

```bash
brew install cmake autoconf automake libtool pkg-config
brew install grpc          # installs grpc@1.78.x
brew install protobuf@33   # must match grpc's protobuf; NOT protobuf@29
brew install abseil spdlog
```

> **Important**: gRPC 1.78 links against protobuf@33 and abseil 20260107. Using protobuf@29 causes a runtime crash ("File already exists in database") due to two incompatible protobuf instances being loaded.

**On Linux:**

Follow the official gRPC C++ quickstart: https://grpc.io/docs/languages/cpp/quickstart/

### Build

```bash
cd 2pc
mkdir build && cd build

cmake -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/grpc;/opt/homebrew/opt/protobuf@33;/opt/homebrew/opt/abseil" ..
cmake --build . -j$(sysctl -n hw.logicalcpu)
```

On Linux with a local gRPC install at `$MY_INSTALL_DIR`:

```bash
cmake -DCMAKE_PREFIX_PATH=$MY_INSTALL_DIR ..
cmake --build . -j$(sysctl -n hw.logicalcpu)
```

### Run

**Input CSV format:**

<p align="center">
  <img src="https://github.com/user-attachments/assets/4dafcffe-2565-41bf-9332-ba885e373e87">
</p>

Each "set" in the CSV is processed as a batch. The `disconnected` column lists servers to treat as unreachable for that set. The `leaders` column lists the preferred leader for each cluster.

**Launch the driver:**

```bash
cd build
./driver <num_clusters> <cluster_size> <csv_filepath> [config_filepath]
```

Example (3 clusters, 3 servers each):

```bash
./driver 3 3 ../test/transactions.csv
```

With a custom address config:

```bash
./driver 3 3 ../test/transactions.csv ../test/config.txt
```

**Config file format** (`config.txt`):

```
S1  localhost:50051
S2  localhost:50052
S3  localhost:50053
S4  localhost:50054
S5  localhost:50055
S6  localhost:50056
S7  localhost:50057
S8  localhost:50058
S9  localhost:50059
```

**Interactive commands:**

| Command              | Description                                      |
|----------------------|--------------------------------------------------|
| `process`            | Read and process the next transaction set        |
| `printBalance <id>`  | Print balance of client with given ID            |
| `printDatastore`     | Print all balances across the cluster            |
| `printPerformance`   | Print throughput and latency statistics          |
| `exit`               | Shut down all servers and exit                   |

**Clean up between runs** (if restarting after a crash):

```bash
pkill -9 -f "build/server"
rm -rf S{1..9}_db S{1..9}.wal
```

---

## Project Structure

```
2pc/
├── src/
│   ├── driver.cc                   # Entry point: CLI driver, mainloop
│   ├── constants.h                 # Cluster/shard mapping constants
│   ├── client/
│   │   ├── client.h                # Client class: stubs, latency tracking
│   │   └── client.cc               # processTransactions, consumeReplies, printPerformance
│   ├── server/
│   │   ├── server.h                # ServerImpl: state, ballot, locks, timers
│   │   ├── server.cc               # HandleRPCs event loop, Paxos phases, 2PC logic
│   │   ├── wal.h                   # WAL interface
│   │   ├── wal.cc                  # WAL read/write/recover
│   │   ├── in_call.cc              # Incoming async RPC call wrappers
│   │   └── out_call.cc             # Outgoing async RPC call wrappers
│   ├── types/
│   │   └── types.h                 # Shared type definitions (WALEntry, Transaction, etc.)
│   └── utils/
│       ├── utils.h / utils.cc      # Setup, server launch, loadConfig
│       ├── csv_reader.h / .cc      # CSV transaction set parser
│       └── commands_parser.h / .cc # CLI command parser
├── protos/
│   └── tpc.proto                   # Protobuf service and message definitions
├── third_party/
│   ├── leveldb/                    # LevelDB submodule
│   └── spdlog/                     # spdlog submodule
├── test/
│   └── transactions.csv            # Example transaction workload
├── build/
│   └── metrics_5runs.tsv           # Benchmark results (5 runs)
├── CMakeLists.txt
└── common.cmake
```

---

## Design Notes

**Why single-threaded server?** All server state (balances, locks, WAL index, Paxos log) is mutated only inside `HandleRPCs()`, which runs on one thread. This eliminates lock contention and race conditions without any application-level synchronization. gRPC's internal thread pool handles I/O; the business logic never touches shared state from multiple threads simultaneously.

**Why LevelDB over WAL for balances?** The WAL is append-only and was designed for 2PC durability (rollback support), not as an authoritative balance store. LevelDB provides O(1) point lookups with crash-safe writes. On recovery, LevelDB gives the correct committed balance directly; the WAL is only scanned to rebuild the pending-prepare index and Paxos metadata.

**Why system_clock for intra-shard latency?** The TID doubles as a timestamp (nanoseconds since epoch). Because `system_clock` is used at submission and the server echoes TID back in the reply, measuring `system_clock::now() - tid` on the client gives correct end-to-end latency without storing per-transaction state on the client for intra-shard transactions. Cross-shard uses `steady_clock` for intervals since the coordinator stores `start_ns` explicitly.
