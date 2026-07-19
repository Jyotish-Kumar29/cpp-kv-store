# Scripts

This directory contains utility bash scripts to streamline the development, testing, and execution workflow for the project.

## Available Scripts

*   **`clean_setup.sh`**: 
    Wipes the CMake `build/` directory and removes any generated data (like AOF files or test outputs) to ensure a clean slate.
    
*   **`run_server.sh`**: 
    Automates the build process (invoking CMake) and launches the compiled `kvstore` binary. It handles configuring the build if it hasn't been done yet.

*   **`run_testing_pipeline.sh`**: 
    An end-to-end orchestration script that:
    1. Compiles the test binaries.
    2. Runs offline GTest and GBenchmark suites.
    3. Runs Python-based network integrity and chaos tests.
    4. Executes latency analytics and throughput stress tests.
    5. Performs an endurance soak test and generates telemetry graphs.
