#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

echo "Configuring tests and benchmarks..."
cmake -S . -B build/tests_n_benchmarks \
    -DCMAKE_BUILD_TYPE=Release

echo "Building test and benchmark binaries..."

cmake --build build/tests_n_benchmarks \
    --target \
        test_kv_store \
        test_utils \
        test_user_book \
        test_persistence \
        test_full_flow \
        test_server_client \
        test_scalability \
        concurrency_stress \
        user_stress \
        disconnect_test \
        bench_internal \
        cold_server_latency \
        steady_server_latency \
        bench_throughput \
        bench_endurance \
    --parallel

echo "Done: build/tests_n_benchmarks/"