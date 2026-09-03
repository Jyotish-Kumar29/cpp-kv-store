#include <benchmark/benchmark.h>

#include "KVStore.hpp"

// Global store for benchmarking. Constructed non-persistent so these
// benchmarks measure pure in-memory KVStore performance, not disk I/O
// from AOF writes.
KVStore global_store(false);

// 1. SET throughput — measures raw write rate into the in-memory hash map under
//    concurrent load. Each thread writes to its own key space to avoid conflicts.
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

        global_store.set(key, val, response);
    }
}
// Scaled from 1 → 8 → 32 → 100 threads to expose contention on the write mutex.
BENCHMARK(BM_KVStore_Set)->Threads(1)->Threads(8)->Threads(32)->Threads(100);

// 2. GET throughput — all threads read the same "hot_key" to maximally stress
//    std::shared_mutex read concurrency. Shows whether shared_lock scales with
//    thread count or serializes under contention.
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
// Scaled from 1 → 8 → 32 → 100 threads to observe shared_lock read scalability.
BENCHMARK(BM_KVStore_Get)->Threads(1)->Threads(8)->Threads(32)->Threads(100);

// 3. DEL throughput — each iteration re-inserts the key during PauseTiming so
//    every timed call hits a real delete, not a NOT_FOUND miss.
static void BM_KVStore_Del(benchmark::State& state) {
    std::string response;
    std::string key_prefix = "del_key_" + std::to_string(state.thread_index()) + "_";
    int i = 0;

    for (auto _ : state) {
        // Pause to (re)insert the key each iteration -- DEL consumes the
        // key, so without this every iteration after the first would just
        // measure a NOT_FOUND miss instead of a real delete.
        state.PauseTiming();
        std::string key = key_prefix + std::to_string(i++);
        global_store.set(key, "value", response);
        state.ResumeTiming();

        global_store.del(key, response);
    }
}
// Scaled from 1 → 8 → 32 → 100 threads to observe lock contention on delete.
BENCHMARK(BM_KVStore_Del)->Threads(1)->Threads(8)->Threads(32)->Threads(100);