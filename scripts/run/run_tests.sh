#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${1:-build/tests_n_benchmarks}"
MODE="${2:-normal}"

PORT=8080

apply_sanitizer_environment() {
    case "$MODE" in
        normal)
            ;;

        asan)
            export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"
            export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
            ;;

        tsan)
            export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"
            ;;

        *)
            echo "ERROR: Unknown mode: $MODE" >&2
            exit 1
            ;;
    esac
}

apply_sanitizer_environment


echo "============================================================"
echo "                      RUNNING TESTS                         "
echo "============================================================"
echo "Build: $BUILD_DIR"
echo "Mode:  $MODE"
echo "============================================================"


if [[ "$MODE" == "normal" ]]; then

    echo -e "\n[UNIT] test_kv_store..."
    "$BUILD_DIR/tests/test_kv_store"

    echo -e "\n[UNIT] test_utils..."
    "$BUILD_DIR/tests/test_utils"

    echo -e "\n[UNIT] test_user_book..."
    "$BUILD_DIR/tests/test_user_book"

    echo -e "\n[UNIT] test_persistence..."
    "$BUILD_DIR/tests/test_persistence"

    echo -e "\n[UNIT] test_data_integrity.py..."
    ./scripts/run/manage_server.sh start "$PORT" false
    python3 tests/unit/test_data_integrity.py
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[INTEGRATION] test_full_flow..."
    ./scripts/run/manage_server.sh start "$PORT" false
    "$BUILD_DIR/tests/test_full_flow"
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[INTEGRATION] test_server_client..."
    ./scripts/run/manage_server.sh start "$PORT" false
    "$BUILD_DIR/tests/test_server_client"
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[CHAOS] TCP_chaos_test.py..."
    ./scripts/run/manage_server.sh start "$PORT" false
    python3 tests/chaos/TCP_chaos_test.py
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[CHAOS] malformed_protocol.py..."
    ./scripts/run/manage_server.sh start "$PORT" false
    python3 tests/chaos/malformed_protocol.py
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[CHAOS] disconnect_test..."
    ./scripts/run/manage_server.sh start "$PORT" false
    "$BUILD_DIR/tests/disconnect_test"
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[STRESS] scalability_test..."
    ./scripts/run/manage_server.sh start "$PORT" false
    "$BUILD_DIR/tests/test_scalability"
    ./scripts/run/manage_server.sh stop "$PORT"

    echo -e "\n[STRESS] concurrency_stress..."
    "$BUILD_DIR/tests/concurrency_stress"

    echo -e "\n[STRESS] user_stress..."
    "$BUILD_DIR/tests/user_stress"


elif [[ "$MODE" == "asan" ]]; then

    echo -e "\n[ASan+UBSan] test_kv_store..."
    "$BUILD_DIR/tests/test_kv_store"

    echo -e "\n[ASan+UBSan] test_utils..."
    "$BUILD_DIR/tests/test_utils"

    echo -e "\n[ASan+UBSan] test_user_book..."
    "$BUILD_DIR/tests/test_user_book"

    echo -e "\n[ASan+UBSan] test_persistence..."
    "$BUILD_DIR/tests/test_persistence"

    echo -e "\n[ASan+UBSan] test_full_flow..."
    "$BUILD_DIR/tests/test_full_flow"

    echo -e "\n[ASan+UBSan] test_server_client..."
    "$BUILD_DIR/tests/test_server_client"

    echo -e "\n[ASan+UBSan] disconnect_test..."
    "$BUILD_DIR/tests/disconnect_test"

    echo -e "\n[ASan+UBSan] concurrency_stress..."
    "$BUILD_DIR/tests/concurrency_stress"

    echo -e "\n[ASan+UBSan] user_stress..."
    "$BUILD_DIR/tests/user_stress"


elif [[ "$MODE" == "tsan" ]]; then

    echo -e "\n[TSan] test_kv_store..."
    "$BUILD_DIR/tests/test_kv_store"

    echo -e "\n[TSan] test_user_book..."
    "$BUILD_DIR/tests/test_user_book"

    echo -e "\n[TSan] concurrency_stress..."
    "$BUILD_DIR/tests/concurrency_stress"

    echo -e "\n[TSan] user_stress..."
    "$BUILD_DIR/tests/user_stress"

fi


echo
echo "============================================================"
echo "                  ALL TESTS COMPLETED                       "
echo "============================================================"