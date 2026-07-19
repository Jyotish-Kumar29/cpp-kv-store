#include <benchmark/benchmark.h>

#include "../include/KVStore.hpp"

// Global store for benchmarking
KVStore global_store;

// 1. Benchmark Throughput of SET operations
static void BM_KVStore_Set(benchmark::State& state) {
    std::string response;
    // state.thread_index() gives each thread a unique ID to avoid key collisions
    std::string key_prefix = "key_" + std::to_string(state.thread_index()) + "_";
    int i = 0;

    for (auto _ : state) {
        // We pause timing to build the strings so string creation
        // doesn't skew our database benchmark
        state.PauseTiming();
        std::string key = key_prefix + std::to_string(i);
        std::string val = "value_" + std::to_string(i);
        i++;
        state.ResumeTiming();

        global_store.set(std::move(key), std::move(val), response);
    }
}
// Run this benchmark scaling from 1 to 32 concurrent threads
BENCHMARK(BM_KVStore_Set)->Threads(1)->Threads(8)->Threads(32)->Threads(100);

// 2. Benchmark Throughput/Latency of GET operations
static void BM_KVStore_Get(benchmark::State& state) {
    std::string response;

    // Setup: Ensure the key exists before benchmarking
    if (state.thread_index() == 0) {
        global_store.set("hot_key", "hot_value", response);
    }

    for (auto _ : state) {
        global_store.get("hot_key", response);
        // This prevents the compiler from optimizing away the operation
        benchmark::DoNotOptimize(response);
    }
}
// Test concurrent reads (This will prove how well std::shared_mutex scales!)
BENCHMARK(BM_KVStore_Get)->Threads(1)->Threads(8)->Threads(32)->Threads(100);