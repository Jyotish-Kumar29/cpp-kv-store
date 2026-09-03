#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build/tests_n_benchmarks}"

PORT=8080

AOF_FILE="$ROOT/data/kvstore.aof"

# Deletes the AOF file so each persistent-mode run starts from an empty store,
# preventing data from one benchmark bleeding into the next.
reset_aof() {
    rm -f "$AOF_FILE"
}

# Ensure the server is stopped even if the script exits early (error or Ctrl-C).
trap './scripts/run/manage_server.sh stop '"$PORT"' >/dev/null 2>&1 || true' EXIT


echo "============================================================"
echo "                    RUNNING BENCHMARKS                      "
echo "============================================================"
echo "Build: $BUILD_DIR"
echo "Modes: non-persistent + persistent"
echo "============================================================"

reset_aof
echo -e "\n[INTERNAL] bench_internal..."
"$BUILD_DIR/benchmarks/bench_internal"


echo -e "\n[NETWORK] cold_server_latency (non-persistent)..."
./scripts/run/manage_server.sh start "$PORT" false
"$BUILD_DIR/benchmarks/cold_server_latency"
./scripts/run/manage_server.sh stop "$PORT"

echo -e "\n[NETWORK] cold_server_latency (persistent)..."
./scripts/run/manage_server.sh start "$PORT" true
"$BUILD_DIR/benchmarks/cold_server_latency"
./scripts/run/manage_server.sh stop "$PORT"
reset_aof
 
 
echo -e "\n[NETWORK] steady_server_latency (non-persistent)..."
./scripts/run/manage_server.sh start "$PORT" false
"$BUILD_DIR/benchmarks/steady_server_latency"
./scripts/run/manage_server.sh stop "$PORT"
 
 
echo -e "\n[NETWORK] steady_server_latency (persistent)..."
./scripts/run/manage_server.sh start "$PORT" true
"$BUILD_DIR/benchmarks/steady_server_latency"
./scripts/run/manage_server.sh stop "$PORT"
reset_aof


echo -e "\n[NETWORK] bench_throughput (non-persistent)..."
./scripts/run/manage_server.sh start "$PORT" false
"$BUILD_DIR/benchmarks/bench_throughput"
./scripts/run/manage_server.sh stop "$PORT"

echo -e "\n[NETWORK] bench_throughput (persistent)..."
./scripts/run/manage_server.sh start "$PORT" true
"$BUILD_DIR/benchmarks/bench_throughput"
./scripts/run/manage_server.sh stop "$PORT"
reset_aof


echo -e "\n[NETWORK] bench_endurance (non-persistent)..."
./scripts/run/manage_server.sh start "$PORT" false
"$BUILD_DIR/benchmarks/bench_endurance"
./scripts/run/manage_server.sh stop "$PORT"

echo -e "\n[NETWORK] bench_endurance (persistent)..."
./scripts/run/manage_server.sh start "$PORT" true
"$BUILD_DIR/benchmarks/bench_endurance"
./scripts/run/manage_server.sh stop "$PORT"
reset_aof


echo
echo "============================================================"
echo "                BENCHMARKS COMPLETED                        "
echo "============================================================"