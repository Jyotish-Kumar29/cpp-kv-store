#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

mkdir -p docs/tests_logs

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="docs/tests_logs/run_all_${TIMESTAMP}.log"


run_normal() {
    echo
    echo "################################################################"
    echo "#                    1/3 NORMAL RUN                           #"
    echo "################################################################"

    "$SCRIPT_DIR/run_tests.sh" \
        build/tests_n_benchmarks \
        normal

    "$SCRIPT_DIR/run_benchmarks.sh" \
        build/tests_n_benchmarks
}


run_asan_ubsan() {
    echo
    echo "################################################################"
    echo "#                 2/3 ASAN + UBSAN RUN                       #"
    echo "################################################################"

    "$SCRIPT_DIR/run_tests.sh" \
        build/asan+ubsan \
        asan
}


run_tsan() {
    echo
    echo "################################################################"
    echo "#                     3/3 TSAN RUN                            #"
    echo "################################################################"

    "$SCRIPT_DIR/run_tests.sh" \
        build/tsan \
        tsan
}


{
    echo "============================================================"
    echo "                 KV-STORE COMPLETE PIPELINE                 "
    echo "============================================================"
    echo "Started: $(date)"
    echo

    "$ROOT/scripts/build/build_all.sh"

    run_normal
    run_asan_ubsan
    run_tsan

    echo
    echo "============================================================"
    echo "                 COMPLETE PIPELINE PASSED                   "
    echo "============================================================"
    echo "Finished: $(date)"
    echo
    echo "Normal:       PASS"
    echo "ASan + UBSan: PASS"
    echo "TSan:         PASS"
    echo

} 2>&1 | tee "$LOG_FILE"


echo
echo "Full log saved to: $LOG_FILE"