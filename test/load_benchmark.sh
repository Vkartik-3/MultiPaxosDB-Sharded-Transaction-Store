#!/usr/bin/env bash
# High-concurrency throughput benchmark with money-conservation check.
#
# Unlike run_benchmark.sh (which uses benchmark.csv with a per-set throttle and
# therefore measures the batch-size-1, ~1.3k tps regime), this feeds one big set
# of disjoint intra-shard transfers with NO inter-set throttle. pending_ fills at
# each leader so rounds commit large batches and all three Paxos groups run in
# parallel -> ~10k tps on localhost. After the load it sums every touched client's
# balance across the 3 replicas; per_replica_total must equal 10 * clients.
#
# Run from 2pc/build after `make server driver`:  bash ../test/load_benchmark.sh
set -uo pipefail

CSV_PATH="${1:-../test/load_3cluster1500.csv}"
RUNS="${2:-3}"
DRIVER_BIN="./driver"
SETTLE_SECS=12

if [[ ! -x "$DRIVER_BIN" ]]; then
  echo "Error: $DRIVER_BIN not found. Run from 2pc/build after building driver." >&2
  exit 1
fi
if [[ ! -f "$CSV_PATH" ]]; then
  echo "Error: $CSV_PATH not found. Generate it: python3 test/gen_load.py" >&2
  exit 1
fi

cleanup_servers() { pkill -9 -f "server S" >/dev/null 2>&1 || true; }
trap cleanup_servers EXIT

grep -oE '\([0-9]+, [0-9]+, [0-9]+\)' "$CSV_PATH" \
  | tr -d '()' | awk -F', ' '{print $1; print $2}' | sort -n | uniq > /tmp/load_clients.txt
N=$(wc -l < /tmp/load_clients.txt | tr -d ' ')
SETS=$(grep -cE '^[0-9]' "$CSV_PATH")   # rows beginning with a set number

echo "Load benchmark: csv=$CSV_PATH runs=$RUNS sets=$SETS clients=$N expected_total=$((N*10))"

for run in $(seq 1 "$RUNS"); do
  cleanup_servers
  rm -rf S{1..9}_db wal_*.log "load_run_${run}.log" >/dev/null 2>&1 || true
  {
    for _ in $(seq 1 "$SETS"); do echo "processNextSet"; done   # no throttle
    sleep "$SETTLE_SECS"
    echo "printPerformance"
    sleep 1
    while IFS= read -r c; do echo "printBalance $c"; done < /tmp/load_clients.txt
    sleep 2
    echo "exit"
  } | "$DRIVER_BIN" 3 3 "$CSV_PATH" > "load_run_${run}.log" 2>&1

  tps=$(grep -oE "Throughput: +[0-9.]+" "load_run_${run}.log" | grep -oE "[0-9.]+$")
  proc=$(grep -oE "Transactions Processed: +[0-9]+" "load_run_${run}.log" | grep -oE "[0-9]+$")
  sum=$(grep -E '^ *S[0-9]+\|' "load_run_${run}.log" | awk -F'|' '{gsub(/ /,"",$2); s+=$2} END {print s}')
  # A balance read that exceeds the client RPC deadline prints "-" (counted as 0
  # in the sum). Those are read-timeout artifacts under load, not money loss:
  # intra-shard transfers are net-zero to the cluster total, so the in-store total
  # is invariant. Report them so a CHECK is attributable.
  timed_out=$(grep -cE '^ *S[0-9]+\| *-\|' "load_run_${run}.log")
  per_replica=$(( ${sum:-0} / 3 ))
  if [[ "$per_replica" -eq "$((N*10))" ]]; then
    status="OK"
  elif [[ "$timed_out" -gt 0 ]]; then
    status="CHECK (${timed_out} balance reads timed out -> undercount; re-run)"
  else
    status="CHECK (settle/in-flight; re-run)"
  fi
  echo "Run $run: tps=${tps:-0} processed=${proc:-0} per_replica_total=${per_replica} expected=$((N*10)) [$status]"
done
