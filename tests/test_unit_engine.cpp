#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "../include/KVStore.hpp"
#include "../include/CommandParser.hpp"

// 1. Basic Unit Tests (Testing the Data Structure directly)
TEST(KVStoreTest, BasicSetAndGet) {
    KVStore store;
    std::string response;

    store.set("user1", "jk", response);
    EXPECT_EQ(response, "OK\n");

    store.get("user1", response);
    EXPECT_EQ(response, "jk\n");

    store.get("missing_key", response);
    EXPECT_EQ(response, "NOT FOUND\n");
}

TEST(KVStoreTest, BasicDelete) {
    KVStore store;
    std::string response;

    store.set("temp", "data", response);
    store.del("temp", response);
    EXPECT_EQ(response, "OK\n");

    store.get("temp", response);
    EXPECT_EQ(response, "NOT FOUND\n");
}

// 2. Concurrency / Integrity Test (Replaces your Python integrity test)
// This proves your std::shared_mutex works under heavy multithreaded load.
TEST(KVStoreTest, ConcurrentWritesAndReads) {
    KVStore store;
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
                store.set(std::move(key), std::move(val), response);
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

// 3. Command Parser Tests
TEST(CommandParserTest, ValidCommands) {
    std::string type, key, val;
    EXPECT_TRUE(parse_command("SET my_key my_val\n", type, key, val));
    EXPECT_EQ(type, "SET");
    EXPECT_EQ(key, "my_key");
    EXPECT_EQ(val, "my_val");

    EXPECT_TRUE(parse_command("GET my_key\n", type, key, val));
    EXPECT_EQ(type, "GET");
    EXPECT_EQ(key, "my_key");

    EXPECT_TRUE(parse_command("DEL my_key\n", type, key, val));
    EXPECT_EQ(type, "DEL");
    EXPECT_EQ(key, "my_key");
}

TEST(CommandParserTest, InvalidCommands) {
    std::string type, key, val;
    EXPECT_FALSE(parse_command("SET my_key\n", type, key, val));
    EXPECT_FALSE(parse_command("GET my_key with spaces\n", type, key, val));
    EXPECT_FALSE(parse_command("UNKNOWN_CMD key\n", type, key, val));
}