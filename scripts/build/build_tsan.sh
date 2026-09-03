#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

echo "Configuring TSan build..."

cmake -S . -B build/tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"

echo "Building TSan test targets..."

# Only build tests where thread-race detection is meaningful.
cmake --build build/tsan \
    --target \
        test_kv_store \
        test_user_book \
        concurrency_stress \
        user_stress \
    --parallel

echo "Done: build/tsan/"