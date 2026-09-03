#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

./scripts/build/build_tsan.sh

BUILD_DIR="build/tsan/tests"

echo -e "\n[TSan] test_kv_store (exercises KVStore's shared_mutex under concurrency)..."
"./$BUILD_DIR/test_kv_store"

echo -e "\n[TSan] test_user_book (exercises UserBook's shared_mutex under concurrency)..."
"./$BUILD_DIR/test_user_book"

echo -e "\nAll TSan runs passed.\n"