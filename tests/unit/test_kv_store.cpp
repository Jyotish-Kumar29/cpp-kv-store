// tests/unit/test_kv_store.cpp
// -----------------------------------------------------------------------------
// Unit tests for KVStore: basic SET/GET/DEL correctness, plus a concurrency
// test proving the internal std::shared_mutex protects the map correctly
// under simultaneous multithreaded reads and writes. Runs fully in-process,
// no live server required.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "KVStore.hpp"

// 1. Basic Unit Tests (Testing the Data Structure directly)
TEST(KVStoreTest, BasicSetAndGet) {
    KVStore store(false);  // non-persistent: no AOF file touched, deterministic test
    std::string response;

    store.set("user1", "jk", response);
    EXPECT_EQ(response, "OK\n");

    store.get("user1", response);
    EXPECT_EQ(response, "jk\n");

    store.get("missing_key", response);
    EXPECT_EQ(response, "NOT_FOUND\n");
}

TEST(KVStoreTest, BasicDelete) {
    KVStore store(false);
    std::string response;

    store.set("temp", "data", response);
    store.del("temp", response);
    EXPECT_EQ(response, "OK\n");

    store.get("temp", response);
    EXPECT_EQ(response, "NOT_FOUND\n");
}

TEST(KVStoreTest, DeleteMissingKey) {
    KVStore store(false);
    std::string response;

    // Deleting a key that was never set should be a clean no-op response,
    // not a crash or an OK that implies something was actually removed.
    store.del("never_set", response);
    EXPECT_EQ(response, "NOT_FOUND\n");
}

// 2. Concurrency / Integrity Test
// This proves your std::shared_mutex works under heavy multithreaded load:
// 50 threads each write 1,000 unique keys concurrently, then 50 threads
// read them back concurrently. Each thread only touches its own key
// namespace ("key_<thread>_<i>"), so any mismatch on read means the
// underlying map corrupted or lost data under concurrent access, not that
// two threads collided on the same key.
TEST(KVStoreTest, ConcurrentWritesAndReads) {
    KVStore store(false);
    const int num_threads = 50;
    const int keys_per_thread = 1000;
    std::vector<std::thread> threads;

    // Write Phase
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&store, t, keys_per_thread]() {
            std::string response;
            for (int i = 0; i < keys_per_thread; ++i) {
                std::string key = "key_" + std::to_string(t) + "_" + std::to_string(i);
                std::string val = "val_" + std::to_string(i);
                store.set(key, val, response);
            }
        });
    }

    for (auto& thread : threads) thread.join();
    threads.clear();

    // Verify Phase
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&store, t, keys_per_thread]() {
            std::string response;
            for (int i = 0; i < keys_per_thread; ++i) {
                std::string key = "key_" + std::to_string(t) + "_" + std::to_string(i);
                store.get(key, response);
                EXPECT_EQ(response, "val_" + std::to_string(i) + "\n");
            }
        });
    }

    for (auto& thread : threads) thread.join();
}