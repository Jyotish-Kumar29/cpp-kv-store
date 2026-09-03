#!/bin/bash
set -e

# Runs in-process unit tests only — no live server needed.
# For the full ASan suite including integration tests, use run_tests.sh:
#   ./scripts/run/run_tests.sh build/asan+ubsan asan

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

./scripts/build/build_asan.sh

BUILD_DIR="build/asan+ubsan/tests"

echo -e "\n[ASan+UBSan] test_kv_store..."
ASAN_OPTIONS=detect_leaks=1 "./$BUILD_DIR/test_kv_store"

echo -e "\n[ASan+UBSan] test_utils..."
ASAN_OPTIONS=detect_leaks=1 "./$BUILD_DIR/test_utils"

echo -e "\n[ASan+UBSan] test_user_book..."
ASAN_OPTIONS=detect_leaks=1 "./$BUILD_DIR/test_user_book"

echo -e "\n[ASan+UBSan] test_persistence..."
ASAN_OPTIONS=detect_leaks=1 "./$BUILD_DIR/test_persistence"

echo -e "\nAll ASan/UBSan runs passed.\n"