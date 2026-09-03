#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

echo "Configuring ASan + UBSan build..."

cmake -S . -B build/asan+ubsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

echo "Building ASan + UBSan test targets..."

# Only build tests that will actually be executed under ASan + UBSan.
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

echo "Done: build/asan+ubsan/"