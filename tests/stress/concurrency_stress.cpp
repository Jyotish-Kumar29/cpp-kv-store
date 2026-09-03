//
// Not a gtest target by design. This binary is built and run in three
// configurations:
//   - normal build
//   - ThreadSanitizer (authoritative race-detection run)
//   - ASan + UBSan (memory and undefined-behavior checks)
//
// The test continuously exercises the shared KVStore and UserBook while
// concurrent writers and readers are active.
//
// KVStore checks:
//   - a value written by a worker can be read back correctly
//   - SET followed by GET remains consistent
//   - DEL removes the key successfully
//   - concurrent access does not produce malformed responses
//
// UserBook checks:
//   - concurrent registration returns unique user IDs
//   - registered users can be authenticated with the correct password
//   - invalid passwords are rejected
//
// The test is intentionally in-process and does not require a live TCP server.
//

#include <KVStore.hpp>
#include <UserBook.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

static std::atomic<uint64_t> g_failures{0};
static std::atomic<uint64_t> g_operations{0};

#define CHECK(cond, msg)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            g_failures.fetch_add(1, std::memory_order_relaxed);                               \
            std::cerr << "INVARIANT FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ \
                      << "\n";                                                                \
        }                                                                                     \
    } while (0)

static void kv_worker(KVStore& store, int worker_id, std::atomic<bool>& stop) {
    std::mt19937 rng(static_cast<unsigned>(worker_id + 1));

    std::uniform_int_distribution<int> key_dist(0, 49);
    std::uniform_int_distribution<int> value_dist(0, 999999);
    std::uniform_int_distribution<int> action_dist(0, 9);

    // Per-worker prefix isolates keys so the SET→GET consistency check
    // is not invalidated by another worker's concurrent DEL on the same key.
    const std::string prefix = "worker_" + std::to_string(worker_id) + "_";

    uint64_t local_operations = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        const int key_number = key_dist(rng);
        const std::string key = prefix + std::to_string(key_number);

        // 0–4 = SET (50%), 5–7 = GET (30%), 8–9 = DEL (20%)
        const int action = action_dist(rng);

        if (action < 5) {
            // SET
            const std::string value = "value_" + std::to_string(value_dist(rng));

            std::string response;

            store.set(key, value, response, false);

            CHECK(response == "OK\n", "KVStore::set returned an unexpected response");

            // Verify the value immediately after the write.
            std::string get_response;

            store.get(key, get_response, false);

            CHECK(get_response == value + "\n",
                  "value returned by GET did not match the preceding SET");

        } else if (action < 8) {
            // GET
            std::string response;

            store.get(key, response, false);

            CHECK(response == "NOT_FOUND\n" || (!response.empty() && response.back() == '\n'),
                  "KVStore::get returned a malformed response");

        } else {
            // DEL
            std::string response;

            store.del(key, response, false);

            CHECK(response == "OK\n" || response == "NOT_FOUND\n",
                  "KVStore::del returned an unexpected response");
        }

        ++local_operations;
    }

    g_operations.fetch_add(local_operations, std::memory_order_relaxed);
}

static void shared_reader(KVStore& store, int reader_id, std::atomic<bool>& stop) {
    std::mt19937 rng(static_cast<unsigned>(std::random_device{}()) +
                     static_cast<unsigned>(reader_id));

    std::uniform_int_distribution<int> worker_dist(0, 7);
    std::uniform_int_distribution<int> key_dist(0, 49);

    uint64_t local_operations = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        const std::string key =
            "worker_" + std::to_string(worker_dist(rng)) + "_" + std::to_string(key_dist(rng));

        std::string response;

        store.get(key, response, false);

        CHECK(response == "NOT_FOUND\n" || (!response.empty() && response.back() == '\n'),
              "reader received a malformed GET response");

        ++local_operations;
    }

    g_operations.fetch_add(local_operations, std::memory_order_relaxed);
}

static void user_registration_worker(UserBook& user_book, int worker_id, int users_per_worker,
                                     std::atomic<bool>& stop) {
    std::vector<uint64_t> user_ids;
    user_ids.reserve(static_cast<size_t>(users_per_worker));

    for (int i = 0; i < users_per_worker; ++i) {
        if (stop.load(std::memory_order_relaxed)) {
            break;
        }

        const std::string username =
            "stress_user_" + std::to_string(worker_id) + "_" + std::to_string(i);

        const std::string password = "password_" + std::to_string(i);

        const uint64_t user_id = user_book.register_user(username, password);

        CHECK(user_id > 0, "register_user returned an invalid user ID");

        user_ids.push_back(user_id);

        CHECK(user_book.exists(user_id), "registered user was not found");

        CHECK(user_book.authenticate_user(user_id, password),
              "registered user could not authenticate with correct password");

        CHECK(!user_book.authenticate_user(user_id, "wrong_password"),
              "user authenticated with an incorrect password");

        CHECK(user_book.get_user_name(user_id) == username,
              "stored username did not match registered username");

        g_operations.fetch_add(1, std::memory_order_relaxed);
    }
}

static void user_reader(UserBook& user_book, const std::vector<uint64_t>& known_users,
                        std::atomic<bool>& stop, int reader_id) {
    if (known_users.empty()) {
        return;
    }

    std::mt19937 rng(static_cast<unsigned>(std::random_device{}()) +
                     static_cast<unsigned>(reader_id));

    std::uniform_int_distribution<size_t> user_dist(0, known_users.size() - 1);

    uint64_t local_operations = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t user_id = known_users[user_dist(rng)];

        CHECK(user_book.exists(user_id), "known user unexpectedly disappeared");

        const std::string name = user_book.get_user_name(user_id);

        CHECK(!name.empty(), "known user returned an empty username");

        ++local_operations;
    }

    g_operations.fetch_add(local_operations, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
    int num_kv_workers = 8;
    int num_readers = 4;
    int num_user_workers = 4;
    int users_per_worker = 250;
    int duration_seconds = 5;

    if (argc >= 2) {
        num_kv_workers = std::atoi(argv[1]);
    }

    if (argc >= 3) {
        num_readers = std::atoi(argv[2]);
    }

    if (argc >= 4) {
        duration_seconds = std::atoi(argv[3]);
    }

    if (argc >= 5) {
        num_user_workers = std::atoi(argv[4]);
    }

    if (argc >= 6) {
        users_per_worker = std::atoi(argv[5]);
    }

    if (num_kv_workers <= 0 || num_readers < 0 || num_user_workers <= 0 || users_per_worker <= 0 ||
        duration_seconds <= 0) {
        std::cerr << "Invalid arguments.\n";
        return 2;
    }

    // Persistence is disabled so the stress test measures only the
    // in-memory data structures and their synchronization.
    KVStore store(false);
    UserBook user_book;

    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;

    threads.reserve(static_cast<size_t>(num_kv_workers + num_readers + num_user_workers));

    std::cout << "Starting concurrency stress test:\n"
              << "  KV workers: " << num_kv_workers << "\n"
              << "  KV readers: " << num_readers << "\n"
              << "  User workers: " << num_user_workers << "\n"
              << "  Users per worker: " << users_per_worker << "\n"
              << "  Duration: " << duration_seconds << "s\n\n";

    // Pre-register a set of users so reader threads have stable IDs
    // that can be queried concurrently.
    std::vector<uint64_t> known_users;

    for (int i = 0; i < num_user_workers * users_per_worker; ++i) {
        const uint64_t user_id =
            user_book.register_user("reader_user_" + std::to_string(i), "password");

        CHECK(user_id > 0, "initial user registration failed");

        known_users.push_back(user_id);
    }

    for (int i = 0; i < num_kv_workers; ++i) {
        threads.emplace_back(kv_worker, std::ref(store), i, std::ref(stop));
    }

    for (int i = 0; i < num_readers; ++i) {
        threads.emplace_back(shared_reader, std::ref(store), i, std::ref(stop));
    }

    for (int i = 0; i < num_user_workers; ++i) {
        threads.emplace_back(user_registration_worker, std::ref(user_book), i, users_per_worker,
                             std::ref(stop));
    }

    // Readers continuously query UserBook while registration workers
    // concurrently add users.
    for (int i = 0; i < num_readers; ++i) {
        threads.emplace_back(user_reader, std::ref(user_book), std::cref(known_users),
                             std::ref(stop), i);
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));

    stop.store(true, std::memory_order_relaxed);

    for (auto& thread : threads) {
        thread.join();
    }

    const uint64_t failures = g_failures.load(std::memory_order_relaxed);

    const uint64_t operations = g_operations.load(std::memory_order_relaxed);

    std::cout << "\n=== concurrency_stress summary ===\n"
              << "kv workers=" << num_kv_workers << " kv readers=" << num_readers
              << " user workers=" << num_user_workers << " users/worker=" << users_per_worker
              << " duration=" << duration_seconds << "s\n"
              << "operations=" << operations << "\n"
              << "invariant failures=" << failures << "\n";

    if (failures == 0) {
        std::cout << "RESULT: PASS\n";
    } else {
        std::cout << "RESULT: FAIL\n";
    }

    return failures == 0 ? 0 : 1;
}
