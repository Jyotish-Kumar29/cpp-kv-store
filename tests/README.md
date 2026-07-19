# Testing Suite

This directory contains the testing and benchmarking code for the C++ KV Store. The suite is designed to validate both the correctness and the performance characteristics of the system.

## Contents

### C++ Tests (GTest & GBenchmark)
*   `test_unit_engine.cpp`: Unit tests for the core `KVStore` logic and `CommandParser`.
*   `bench_internal_engine.cpp`: Internal micro-benchmarks for storage operations (Get, Set) across multiple threads.
*   `bench_net_latency.cpp`: Precision latency analysis with and without lock contention.
*   `bench_net_throughput.cpp`: Throughput stress testing over persistent TCP connections.
*   `test_net_endurance.cpp`: Long-running (soak) tests designed to evaluate memory leaks and stability over extended periods (e.g., 10 minutes).

### Python Tests
*   `test_net_integrity.py`: ACID-lite data integrity checks. Validates that bulk writes are accurately persisted and read back.
*   `test_net_stream_chaos.py`: TCP stream resilience testing (e.g., packet fragmentation, sticky packets, random split boundaries).
*   `plot_metrics.py`: Script to generate graphical charts (e.g., PNGs) from endurance telemetry data (CSV).

## Execution

The tests are intended to be run cohesively via the `run_testing_pipeline.sh` script located in the `scripts/` directory, which handles compilation, orchestration, and output formatting.
