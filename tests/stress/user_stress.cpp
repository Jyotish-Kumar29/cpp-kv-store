// UserBook concurrency stress test.
//
// Spawns multiple writer threads (registering users) and reader threads
// (querying users) against a single shared UserBook instance to catch
// data races, deadlocks, and correctness issues under concurrent load.
//
// Usage: ./user_stress [num_registration_threads] [registrations_per_thread] [num_query_threads]

#include <UserBook.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Aggregate failure counters, updated from multiple threads.
// Relaxed atomics are sufficient here since these are just counters —
// no ordering guarantees are needed relative to other memory operations.
static std::atomic<uint64_t> g_registration_failures{0};
static std::atomic<uint64_t> g_auth_failures{0};
static std::atomic<uint64_t> g_operation_failures{0};

// Repeatedly registers unique users, then immediately verifies the write
// is visible via exists(), authenticate_user(), and get_user_name().
// Any mismatch indicates a race condition or lost update in UserBook.
void registration_stress(UserBook& user_book, int thread_id, int num_registrations) {
    for (int i = 0; i < num_registrations; ++i) {
        const std::string username =
            "stress_user_" + std::to_string(thread_id) + "_" + std::to_string(i);

        const uint64_t user_id = user_book.register_user(username, "password");

        if (!user_book.exists(user_id)) {
            g_registration_failures.fetch_add(1, std::memory_order_relaxed);
        }

        if (!user_book.authenticate_user(user_id, "password")) {
            g_auth_failures.fetch_add(1, std::memory_order_relaxed);
        }

        if (user_book.get_user_name(user_id) != username) {
            g_operation_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Hammers UserBook with read-only lookups against a fixed ID range
// (1-1000) to exercise concurrent reads while writers are active.
// Results are discarded; this thread's job is to surface crashes,
// undefined behavior, or lock contention issues, not to check correctness.
void query_stress(UserBook& user_book, int num_queries) {
    std::mt19937 rng(std::random_device{}());

    std::uniform_int_distribution<uint64_t> user_id_dist(1, 1000);

    for (int i = 0; i < num_queries; ++i) {
        const uint64_t user_id = user_id_dist(rng);

        // Intentionally unchecked — purpose is to stress concurrent
        // access, not validate results.
        (void)user_book.exists(user_id);
        (void)user_book.get_user_name(user_id);
    }
}

// Entry point: configures thread counts from CLI args (with sane defaults),
// runs the mixed read/write stress test, and reports pass/fail based on
// whether any correctness violations were observed.
int main(int argc, char** argv) {
    // Defaults: 8 writer threads x 10k registrations each, 4 reader threads.
    int num_registration_threads = 8;
    int registrations_per_thread = 10000;
    int num_query_threads = 4;

    if (argc >= 2) {
        num_registration_threads = std::atoi(argv[1]);
    }

    if (argc >= 3) {
        registrations_per_thread = std::atoi(argv[2]);
    }

    if (argc >= 4) {
        num_query_threads = std::atoi(argv[3]);
    }

    UserBook user_book;

    std::cout << "Starting UserBook stress test...\n"
              << "Registration threads: " << num_registration_threads << '\n'
              << "Registrations per thread: " << registrations_per_thread << '\n'
              << "Query threads: " << num_query_threads << "\n\n";

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_registration_threads + num_query_threads));

    // Registration threads exercise concurrent writes while query threads
    // exercise concurrent reads against the same UserBook instance.
    for (int thread_id = 0; thread_id < num_registration_threads; ++thread_id) {
        threads.emplace_back(registration_stress, std::ref(user_book), thread_id,
                             registrations_per_thread);
    }

    for (int thread_id = 0; thread_id < num_query_threads; ++thread_id) {
        threads.emplace_back(query_stress, std::ref(user_book), registrations_per_thread);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto end = std::chrono::steady_clock::now();

    const auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Snapshot final counts after all threads have joined (no concurrent
    // access here, so relaxed loads are safe and simple).
    const uint64_t registration_failures = g_registration_failures.load(std::memory_order_relaxed);
    const uint64_t auth_failures = g_auth_failures.load(std::memory_order_relaxed);
    const uint64_t operation_failures = g_operation_failures.load(std::memory_order_relaxed);
    const uint64_t total_failures = registration_failures + auth_failures + operation_failures;

    std::cout << "=== UserBook Stress Test Results ===\n"
              << "Total time: " << duration << " ms\n"
              << "Registration failures: " << registration_failures << '\n'
              << "Authentication failures: " << auth_failures << '\n'
              << "Operation failures: " << operation_failures << '\n';

    // Any non-zero failure count means a data race or correctness bug was hit.
    if (total_failures == 0) {
        std::cout << "RESULT: PASS\n";
        return 0;
    }

    std::cout << "RESULT: FAIL\n";

    return 1;
}