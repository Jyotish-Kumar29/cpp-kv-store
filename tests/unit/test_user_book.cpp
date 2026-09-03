// tests/unit/test_user_book.cpp
// -----------------------------------------------------------------------------
// Unit tests for UserBook: registration, existence checks, authentication,
// and name lookups, plus a concurrency test proving registrations under
// simultaneous multithreaded load never collide or get lost.
//
// UserBook had no dedicated test coverage before this file -- it was only
// exercised indirectly through live-server REGISTER/LOGIN traffic in the
// Python integration tests. These tests cover it directly, in-process.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <set>
#include <thread>
#include <vector>

#include "UserBook.hpp"

TEST(UserBookTest, RegisterAssignsIncreasingIds) {
    UserBook book;
    uint64_t id1 = book.register_user("alice", "pw1");
    uint64_t id2 = book.register_user("bob", "pw2");
    EXPECT_LT(id1, id2);
}

TEST(UserBookTest, ExistsReflectsRegistration) {
    UserBook book;
    EXPECT_FALSE(book.exists(999));
    uint64_t id = book.register_user("carol", "pw");
    EXPECT_TRUE(book.exists(id));
}

TEST(UserBookTest, AuthenticateCorrectPassword) {
    UserBook book;
    uint64_t id = book.register_user("dave", "secret");
    EXPECT_TRUE(book.authenticate_user(id, "secret"));
}

TEST(UserBookTest, AuthenticateRejectsWrongPassword) {
    UserBook book;
    uint64_t id = book.register_user("erin", "secret");
    EXPECT_FALSE(book.authenticate_user(id, "wrong"));
}

TEST(UserBookTest, AuthenticateRejectsUnknownUser) {
    UserBook book;
    EXPECT_FALSE(book.authenticate_user(12345, "anything"));
}

TEST(UserBookTest, GetUserNameReturnsRegisteredName) {
    UserBook book;
    uint64_t id = book.register_user("frank", "pw");
    EXPECT_EQ(book.get_user_name(id), "frank");
}

TEST(UserBookTest, GetUserNameReturnsEmptyForUnknownUser) {
    UserBook book;
    EXPECT_EQ(book.get_user_name(999999), "");
}

// Concurrency: many threads registering simultaneously must never assign
// the same user_id twice and must never lose a registration -- this is the
// property the shared_mutex around users_map/next_user_id exists to
// protect, and it had no test proving it held before this file.
//
// Each thread collects its own assigned IDs into ids_per_thread[t] with no
// shared writes between threads (only UserBook's internal state is
// actually contended), so the join afterward is what makes it safe to
// merge everything into a single std::set and check for duplicates.
TEST(UserBookTest, ConcurrentRegistrationsAllUnique) {
    UserBook book;
    const int num_threads = 50;
    const int regs_per_thread = 200;
    std::vector<std::thread> threads;
    std::vector<std::vector<uint64_t>> ids_per_thread(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&book, &ids_per_thread, t, regs_per_thread]() {
            for (int i = 0; i < regs_per_thread; ++i) {
                std::string name = "user_" + std::to_string(t) + "_" + std::to_string(i);
                ids_per_thread[t].push_back(book.register_user(name, "pw"));
            }
        });
    }
    for (auto& th : threads) th.join();

    // Merge all collected IDs and confirm every insertion succeeds --
    // set::insert returning false anywhere means two threads were handed
    // the same user_id.
    std::set<uint64_t> all_ids;
    for (auto& ids : ids_per_thread) {
        for (auto id : ids) {
            auto inserted = all_ids.insert(id);
            EXPECT_TRUE(inserted.second) << "Duplicate user_id assigned: " << id;
        }
    }
    EXPECT_EQ(all_ids.size(), static_cast<size_t>(num_threads * regs_per_thread));
}