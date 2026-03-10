#!/usr/bin/env bash
set -euo pipefail

RUNS=3
SETS=50
DRIVER_BIN="./driver"
CSV_PATH="../test/benchmark.csv"

if [[ ! -x "$DRIVER_BIN" ]]; then
  echo "Error: $DRIVER_BIN not found. Run this script from 2pc/build after building driver." >&2
  exit 1
fi

if [[ ! -f "$CSV_PATH" ]]; then
  echo "Error: $CSV_PATH not found." >&2
  exit 1
fi

cleanup_servers() {
  pkill -9 -f "build/server" >/dev/null 2>&1 || true
  pkill -9 -f "^server " >/dev/null 2>&1 || true
}

get_cluster() {
  local client_id=$1
  echo $(( (client_id - 1) / 1000 + 1 ))
}

count_mix() {
  local intra=0
  local cross=0
  while IFS= read -r line; do
    if [[ $line =~ \(([0-9]+),[[:space:]]*([0-9]+),[[:space:]]*([0-9]+)\) ]]; then
      local sender="${BASH_REMATCH[1]}"
      local receiver="${BASH_REMATCH[2]}"
      local sender_cluster receiver_cluster
      sender_cluster=$(get_cluster "$sender")
      receiver_cluster=$(get_cluster "$receiver")
      if [[ "$sender_cluster" -eq "$receiver_cluster" ]]; then
        intra=$((intra + 1))
      else
        cross=$((cross + 1))
      fi
    fi
  done < "$CSV_PATH"
  echo "$intra $cross"
}

extract_metric() {
  local log_file="$1"
  local key="$2"
  awk -F': *' -v k="$key" '$0 ~ k {v=$2} END {print v}' "$log_file" | awk '{print $1}'
}

read -r INTRA_TXNS CROSS_TXNS < <(count_mix)
TOTAL_TXNS=$((INTRA_TXNS + CROSS_TXNS))

echo "Benchmark config: runs=$RUNS sets=$SETS total_txns=$TOTAL_TXNS intra=$INTRA_TXNS cross=$CROSS_TXNS"

throughputs=()
means=()
p50s=()
p99s=()
wall_times=()
processed=()

trap cleanup_servers EXIT

for run in $(seq 1 "$RUNS"); do
  cleanup_servers
  rm -rf S{1..9}_db wal_*.log "run_${run}.log" >/dev/null 2>&1 || true

  (
    {
      for _ in $(seq 1 "$SETS"); do
        echo "processNextSet"
        sleep 0.04
      done
      sleep 8
      echo "printPerformance"
      sleep 1
      echo "exit"
    } | "$DRIVER_BIN" 3 3 "$CSV_PATH" > "run_${run}.log" 2>&1
  ) &

  pid=$!

  for _ in $(seq 1 180); do
    if grep -q "Latency p99:" "run_${run}.log" 2>/dev/null; then
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 0.5
  done

  kill -9 "$pid" >/dev/null 2>&1 || true
  cleanup_servers

  tx=$(extract_metric "run_${run}.log" "Transactions Processed")
  wall=$(extract_metric "run_${run}.log" "Wall-clock time")
  tps=$(extract_metric "run_${run}.log" "Throughput")
  mean=$(extract_metric "run_${run}.log" "Latency mean")
  p50=$(extract_metric "run_${run}.log" "Latency p50")
  p99=$(extract_metric "run_${run}.log" "Latency p99")

  processed+=("${tx:-0}")
  wall_times+=("${wall:-0}")
  throughputs+=("${tps:-0}")
  means+=("${mean:-0}")
  p50s+=("${p50:-0}")
  p99s+=("${p99:-0}")

  echo "Run $run: tx=${tx:-0} wall=${wall:-0}s tps=${tps:-0} mean=${mean:-0}ms p50=${p50:-0}ms p99=${p99:-0}ms"
done

echo
echo "Averages across $RUNS runs:"
printf "%s\n" "${processed[@]}"   | awk '{s+=$1} END {printf "Transactions Processed: %.2f\n", s/NR}'
printf "%s\n" "${wall_times[@]}"  | awk '{s+=$1} END {printf "Wall-clock time: %.2f s\n", s/NR}'
printf "%s\n" "${throughputs[@]}" | awk '{s+=$1} END {printf "Throughput: %.2f tps\n", s/NR}'
printf "%s\n" "${means[@]}"       | awk '{s+=$1} END {printf "Latency mean: %.2f ms\n", s/NR}'
printf "%s\n" "${p50s[@]}"        | awk '{s+=$1} END {printf "Latency p50: %.2f ms\n", s/NR}'
printf "%s\n" "${p99s[@]}"        | awk '{s+=$1} END {printf "Latency p99: %.2f ms\n", s/NR}'

echo "Mix: intra=$INTRA_TXNS cross=$CROSS_TXNS"
