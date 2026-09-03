// tests/unit/test_persistence.cpp
// -----------------------------------------------------------------------------
// GOAL: Verify KVStore's AOF persistence actually round-trips correctly --
// writes made before a restart are recoverable after one, deletes are
// recoverable as deletes (not just "key never existed"), and non-persistent
// mode genuinely touches no disk.
//
// These tests use an isolated storage_path (see KVStore(bool, std::string))
// so they never read or write the real server's data/kvstore.aof.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <filesystem>

#include "KVStore.hpp"

namespace {

const std::string kTestAofPath = "data/test_persistence.aof";

// Every test starts and ends with a clean slate so runs don't leak state
// into each other, and a failed run doesn't leave a stale file behind for
// the next one to accidentally inherit.
class PersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::remove(kTestAofPath);
    }
    void TearDown() override {
        std::filesystem::remove(kTestAofPath);
    }
};

TEST_F(PersistenceTest, WritesSurviveRestart) {
    std::string response;
    {
        KVStore store(true, kTestAofPath);
        store.set("k1", "v1", response);
        store.set("k2", "v2", response);
        // store goes out of scope here -- destructor flushes and closes
        // the AOF file, simulating a clean server shutdown.
    }

    KVStore reloaded(true, kTestAofPath);
    reloaded.get("k1", response);
    EXPECT_EQ(response, "v1\n");
    reloaded.get("k2", response);
    EXPECT_EQ(response, "v2\n");
}

TEST_F(PersistenceTest, DeletesSurviveRestart) {
    std::string response;
    {
        KVStore store(true, kTestAofPath);
        store.set("temp", "data", response);
        store.del("temp", response);
    }

    // Replay must apply the DEL record, not just skip it -- otherwise a
    // reload would resurrect data the user explicitly removed.
    KVStore reloaded(true, kTestAofPath);
    reloaded.get("temp", response);
    EXPECT_EQ(response, "NOT_FOUND\n");
}

TEST_F(PersistenceTest, LastWriteWinsOnReplay) {
    std::string response;
    {
        KVStore store(true, kTestAofPath);
        store.set("k", "first", response);
        store.set("k", "second", response);
    }

    // Replay must apply AOF records in order and let later writes to the
    // same key overwrite earlier ones, matching live SET semantics.
    KVStore reloaded(true, kTestAofPath);
    reloaded.get("k", response);
    EXPECT_EQ(response, "second\n");
}

TEST_F(PersistenceTest, NonPersistentModeCreatesNoFile) {
    {
        KVStore store(false, kTestAofPath);
        std::string response;
        store.set("ghost", "value", response);
    }
    EXPECT_FALSE(std::filesystem::exists(kTestAofPath));
}

TEST_F(PersistenceTest, PersistentModeCreatesFile) {
    {
        KVStore store(true, kTestAofPath);
        std::string response;
        store.set("k", "v", response);
    }
    EXPECT_TRUE(std::filesystem::exists(kTestAofPath));
}

TEST_F(PersistenceTest, FreshStartWithNoExistingFileWorksNormally) {
    // No SetUp file exists yet for a brand-new path -- constructor must
    // handle "AOF file not found" gracefully rather than treating it as
    // an error.
    KVStore store(true, kTestAofPath);
    std::string response;
    store.get("anything", response);
    EXPECT_EQ(response, "NOT_FOUND\n");
}

}  // namespace