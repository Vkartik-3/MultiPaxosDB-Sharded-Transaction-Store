#!/usr/bin/env python3
"""Generate a high-concurrency intra-shard load CSV for throughput benchmarking.

The standard benchmark.csv scatters a handful of transactions across leaders and
the harness throttles between sets, so each Paxos round commits exactly one
transaction (batch size 1) and throughput is capped by per-round latency. To
exercise batching, this emits one big set of disjoint intra-shard transfers, all
issued back-to-back: pending_ fills at each leader while a round is in flight, so
the next round commits a large batch.

Layout (clusters of 1000 clients each: cluster c == ceil(id/1000)):
  - `pairs_per_cluster` disjoint transfers per cluster, pair k = (2k-1 -> 2k),
    each client used exactly once so there are no intra-batch lock conflicts.
  - Transfers from all clusters are interleaved so every leader (S1, S4, S7) gets
    load from the start and the three Paxos groups batch in parallel.
  - Amount 1; with starting balance 10 every transfer succeeds and money is
    conserved (sender -1, receiver +1 within the same shard).

Usage (from 2pc/):  python3 test/gen_load.py [pairs_per_cluster] [out.csv]
Defaults: 500 pairs/cluster (1500 transfers) -> test/load_3cluster1500.csv
"""
import sys

CLIENTS_PER_CLUSTER = 1000
NUM_CLUSTERS = 3
SERVERS = "[S1, S2, S3, S4, S5, S6, S7, S8, S9]"
LEADERS = "[S1, S4, S7]"


def main():
    pairs_per_cluster = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    out = sys.argv[2] if len(sys.argv) > 2 else "test/load_3cluster1500.csv"
    if pairs_per_cluster > CLIENTS_PER_CLUSTER // 2:
        sys.exit(f"pairs_per_cluster must be <= {CLIENTS_PER_CLUSTER // 2} (disjoint pairs)")

    cluster_pairs = []
    for c in range(NUM_CLUSTERS):
        base = c * CLIENTS_PER_CLUSTER
        cluster_pairs.append([(base + 2 * k - 1, base + 2 * k) for k in range(1, pairs_per_cluster + 1)])

    rows, first = [], True
    for k in range(pairs_per_cluster):
        for c in range(NUM_CLUSTERS):
            s, r = cluster_pairs[c][k]
            if first:
                rows.append(f'1,"({s}, {r}, 1)","{SERVERS}","{LEADERS}"')
                first = False
            else:
                rows.append(f',"({s}, {r}, 1)",,')

    with open(out, "w") as f:
        f.write("\n".join(rows) + "\n")
    print(f"wrote {len(rows)} transfers ({pairs_per_cluster}/cluster x {NUM_CLUSTERS}) to {out}")


if __name__ == "__main__":
    main()
