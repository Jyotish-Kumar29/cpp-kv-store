#!/bin/bash
set -e 

echo "============================================================"
echo "                 KV-STORE TESTING PIPELINE                  "
echo "============================================================"

# ---------------------------------------------------------
# PHASE 1: COMPILE TESTS ONLY
# ---------------------------------------------------------

echo -e "\n[PHASE 1] Compiling Test Binaries..."

# ADDED: Create the directory structure before compiling so the compiler doesn't crash
mkdir -p build/tests
mkdir -p tests/results

# Build Unit Tests (GTest)
g++ -std=c++17 tests/test_unit_engine.cpp src/KVStore.cpp src/CommandParser.cpp \
    -I./include -lgtest -lgtest_main -pthread -o build/tests/test_unit

# Build Internal Benchmarks (Google Benchmark)
g++ -std=c++17 tests/bench_internal_engine.cpp src/KVStore.cpp src/CommandParser.cpp \
    -I./include -lbenchmark -lbenchmark_main -pthread -o build/tests/bench_internal

# Build Client Load Generators
g++ -std=c++17 tests/bench_net_latency.cpp -pthread -o build/tests/bench_latency

# FIXED TYPO: Changed build/test/ to build/tests/
g++ -std=c++17 tests/bench_net_throughput.cpp -pthread -o build/tests/bench_throughput

g++ -std=c++17 tests/test_net_endurance.cpp -pthread -o build/tests/test_endurance

echo "Test compilation successful."

# ---------------------------------------------------------
# PHASE 2: INTERNAL UNIT TESTS & BENCHMARKS (OFFLINE)
# ---------------------------------------------------------
echo -e "\n[PHASE 2] Running Offline Unit Tests (GTest)..."
./build/tests/test_unit

echo -e "\n[PHASE 2] Running Offline Internal Benchmarks (GBench)..."
./build/tests/bench_internal

# # ---------------------------------------------------------
# # PAUSE: START YOUR SERVER NOW
# # ---------------------------------------------------------
# echo -e "\n============================================================"
# echo "[!] OFFLINE TESTS COMPLETE."
# echo "[!] Please ensure your KV-store server is running in another"
# echo "    terminal window before continuing."
# echo "============================================================"
# read -p "Press Enter to begin online network tests..."

# ---------------------------------------------------------
# PHASE 3: CORRECTNESS & CHAOS ENGINEERING (ONLINE)
# ---------------------------------------------------------
echo -e "\n[PHASE 3] Running Integrity Checks (Python)..."
python3 tests/test_net_integrity.py

# (Commented out intentionally based on your script)
echo -e "\n[PHASE 3] Running Stream Chaos & Resilience Tests (Python)..."
python3 tests/test_net_stream_chaos.py

# ---------------------------------------------------------
# PHASE 4: PERFORMANCE & STRESS TESTING (ONLINE)
# ---------------------------------------------------------
echo -e "\n[PHASE 4] Running Latency Analytics..."
./build/tests/bench_latency

echo -e "\n[PHASE 4] Running Throughput Stress Test..."
./build/tests/bench_throughput

# ---------------------------------------------------------
# PHASE 5: ENDURANCE SOAK TEST (ONLINE)
# ---------------------------------------------------------
echo -e "\n[PHASE 5] Running Endurance Soak Test..."
echo "This will take some minutes. Please wait..."
./build/tests/test_endurance

# ---------------------------------------------------------
# PHASE 6: ANALYTICS & PLOTTING
# ---------------------------------------------------------
echo -e "\n[PHASE 6] Generating Endurance Telemetry Graph..."
python3 tests/plot_metrics.py

echo "============================================================"
echo "                PIPELINE COMPLETED SUCCESSFULLY             "
echo "============================================================"