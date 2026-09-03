#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

echo "============================================================"
echo "                 BUILDING ALL CONFIGURATIONS               "
echo "============================================================"

# ---------------------------------------------------------
# 1. SERVER
# ---------------------------------------------------------

echo
echo "[1/4] Building server..."

cmake -S . -B build/server \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build/server --target kvstore --parallel

# Keep manage_server.sh unchanged.
# It expects the server at build/kvstore.
ln -sfn server/kvstore build/kvstore

echo "[1/4] Server build complete: build/server/"


# ---------------------------------------------------------
# 2. NORMAL TESTS + BENCHMARKS
# ---------------------------------------------------------

echo
echo "[2/4] Building tests and benchmarks..."

cmake -S . -B build/tests_n_benchmarks \
    -DCMAKE_BUILD_TYPE=Release

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

echo "[2/4] Tests and benchmarks build complete: build/tests_n_benchmarks/"


# ---------------------------------------------------------
# 3. ASAN + UBSAN
# ---------------------------------------------------------

echo
echo "[3/4] Building ASan + UBSan configuration..."

cmake -S . -B build/asan+ubsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

# Only build tests that will actually be executed under ASan/UBSan.
cmake --build build/asan+ubsan \
    --target \
        test_kv_store \
        test_utils \
        test_user_book \
        test_persistence \
        test_full_flow \
        test_server_client \
        concurrency_stress \
        user_stress \
        disconnect_test \
    --parallel

echo "[3/4] ASan + UBSan build complete: build/asan+ubsan/"


# ---------------------------------------------------------
# 4. TSAN
# ---------------------------------------------------------

echo
echo "[4/4] Building TSan configuration..."

cmake -S . -B build/tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"

# Only build tests where race detection is meaningful.
cmake --build build/tsan \
    --target \
        test_kv_store \
        test_user_book \
        concurrency_stress \
        user_stress \
    --parallel

echo "[4/4] TSan build complete: build/tsan/"


echo
echo "============================================================"
echo "                 ALL BUILDS COMPLETED                      "
echo "============================================================"
echo
echo "Server:           build/server/"
echo "Tests/Benchmarks: build/tests_n_benchmarks/"
echo "ASan + UBSan:     build/asan+ubsan/"
echo "TSan:             build/tsan/"
echo