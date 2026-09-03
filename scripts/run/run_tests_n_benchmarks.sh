#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

mkdir -p docs/tests_logs
LOG_FILE="docs/tests_logs/all_tests_results_$(date +%Y%m%d_%H%M%S).log"

{
    echo "============================================================"
    echo "                 KV-STORE FULL TEST + BENCH RUN              "
    echo "============================================================"

    ./scripts/build/build_all.sh

    ./scripts/run/run_tests.sh
    ./scripts/run/run_benchmarks.sh

    echo "============================================================"
    echo "                       RUN COMPLETE                          "
    echo "============================================================"
} 2>&1 | tee "$LOG_FILE"

echo -e "\nFull log saved to: $LOG_FILE\n"