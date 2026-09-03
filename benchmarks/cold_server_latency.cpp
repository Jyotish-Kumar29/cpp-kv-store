// cold_server_latency.cpp
//
// Whole-server TCP latency benchmark in cold/growing-state mode.
//
// The server starts empty. Multiple clients continuously send a randomized
// mixed SET/GET/DEL workload over their own registered (namespaced) user.
// Each request is timed independently from the first send() until the
// complete newline-terminated response is received.
//
// The reported "key count" is the cumulative number of successfully
// accepted SET requests across all workers combined, not the number of
// currently live keys on the server -- DEL activity means live key count
// can be lower than this at any given moment. Since keys are namespaced
// per user, this number also does not correspond to any single user's
// keyspace size; it's a proxy for "how much total data the server is
// holding," which is what's expected to affect latency.
//
// SCOPE: All measurements are over loopback (127.0.0.1). Latency figures
// reflect server-side processing time plus kernel round-trip only — not
// real-network conditions. Expect significantly higher numbers over LAN/WAN.
//
// Usage:
//   ./cold_server_latency [SET GET DEL]
//
// Provide either no percentages (uses defaults) or all 3, summing to 100.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

// -----------------------------------------------------------------------------
// Benchmark configuration
// -----------------------------------------------------------------------------

constexpr std::array<uint64_t, 5> kTiers = {100, 1000, 10000, 100000, 1000000};

constexpr size_t kMaxResponseBytes = 1ULL * 1024ULL * 1024ULL;

// Fixed payload size for SET values, so request cost is dominated by
// server-side work rather than varying I/O size.
constexpr size_t kValueSize = 64;

// Caps how many locally-known live key IDs a worker keeps around for
// GET/DEL targeting. Bounds memory; does not affect correctness.
constexpr size_t kMaxKnownKeys = 50000;

// -----------------------------------------------------------------------------
// Workload operations
// -----------------------------------------------------------------------------

enum class Op : int { SET = 0, GET, DEL };

// Updated by run_benchmark() for each workload configuration.
std::array<double, 3> kOpWeights{};

// -----------------------------------------------------------------------------
// Shared state
// -----------------------------------------------------------------------------

struct SharedState {
    std::atomic<uint64_t> accepted_sets{0};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> next_progress_mark{0};

    std::mutex print_mutex;
};

// -----------------------------------------------------------------------------
// Latency sample
// -----------------------------------------------------------------------------

struct Sample {
    double latency_us = 0.0;
};

// -----------------------------------------------------------------------------
// Networking
// -----------------------------------------------------------------------------

int connect_to_server(const std::string& host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

bool send_all(int fd, std::string_view data) {
    size_t sent_total = 0;

    while (sent_total < data.size()) {
        ssize_t sent = send(fd, data.data() + sent_total, data.size() - sent_total, MSG_NOSIGNAL);

        if (sent > 0) {
            sent_total += static_cast<size_t>(sent);
            continue;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

// Receives one newline-terminated protocol response. The latency measurement
// ends when the complete response becomes available to the benchmark client.
bool receive_line(int fd, std::string& response) {
    response.clear();
    response.reserve(256);

    char buffer[4096];

    while (response.size() <= kMaxResponseBytes) {
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);

        if (received > 0) {
            response.append(buffer, static_cast<size_t>(received));

            if (response.find('\n') != std::string::npos) {
                return true;
            }

            continue;
        }

        if (received < 0 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Parsing helpers
// -----------------------------------------------------------------------------

bool parse_u64(std::string_view text, uint64_t& out) {
    if (text.empty()) {
        return false;
    }

    uint64_t value = 0;

    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }

        const uint64_t digit = static_cast<uint64_t>(c - '0');

        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }

        value = value * 10 + digit;
    }

    out = value;
    return true;
}

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

// REGISTER also authenticates the connection -- no separate LOGIN is needed
// before issuing SET/GET/DEL on the same socket.
bool register_user(int fd, const std::string& username, uint64_t& out_user_id) {
    std::string request = "REGISTER|" + username + "|pw\n";

    if (!send_all(fd, request)) {
        return false;
    }

    std::string response;

    if (!receive_line(fd, response)) {
        return false;
    }

    if (response.rfind("REGISTER_OK|", 0) != 0) {
        return false;
    }

    std::string_view sv(response);
    sv.remove_prefix(std::strlen("REGISTER_OK|"));

    size_t nl = sv.find('\n');

    if (nl != std::string_view::npos) {
        sv = sv.substr(0, nl);
    }

    return parse_u64(sv, out_user_id) && out_user_id != 0;
}

// -----------------------------------------------------------------------------
// Formatting
// -----------------------------------------------------------------------------

std::string format_commas(uint64_t value) {
    std::string digits = std::to_string(value);
    std::string out;

    int since_comma = 0;

    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (since_comma == 3) {
            out.push_back(',');
            since_comma = 0;
        }

        out.push_back(*it);
        ++since_comma;
    }

    std::reverse(out.begin(), out.end());

    return out;
}

std::string range_label(uint64_t lo, uint64_t hi) {
    if (lo == 0) {
        return "0-" + format_commas(hi);
    }

    return format_commas(lo) + "-" + format_commas(hi);
}

// -----------------------------------------------------------------------------
// Latency statistics
// -----------------------------------------------------------------------------

double percentile(const std::vector<Sample>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }

    const size_t index = static_cast<size_t>(p * static_cast<double>(values.size() - 1));

    return values[index].latency_us;
}

void print_latency_row(const std::string& label, const std::vector<Sample>& values) {
    constexpr int kLabelWidth = 22;
    constexpr int kColWidth = 13;

    std::cout << std::left << std::setw(kLabelWidth) << label;

    if (values.empty()) {
        std::cout << "(no samples)\n";
        return;
    }

    std::vector<Sample> sorted = values;

    std::sort(sorted.begin(), sorted.end(),
              [](const Sample& a, const Sample& b) { return a.latency_us < b.latency_us; });

    double avg = 0.0;

    for (const Sample& sample : sorted) {
        avg += sample.latency_us;
    }

    avg /= static_cast<double>(sorted.size());

    std::cout << std::right << std::setw(kColWidth) << sorted.size() << std::fixed
              << std::setprecision(3) << std::setw(kColWidth) << avg << std::setw(kColWidth)
              << percentile(sorted, 0.50) << std::setw(kColWidth) << percentile(sorted, 0.90)
              << std::setw(kColWidth) << percentile(sorted, 0.95) << std::setw(kColWidth)
              << percentile(sorted, 0.99) << std::setw(kColWidth) << percentile(sorted, 0.999)
              << std::setw(kColWidth) << percentile(sorted, 0.9999) << std::setw(kColWidth)
              << sorted.back().latency_us << '\n';
}

void print_table_header(const std::string& title) {
    constexpr int kLabelWidth = 22;
    constexpr int kColWidth = 13;

    std::cout << '\n' << title << "\n";

    std::cout << std::left << std::setw(kLabelWidth) << "Cumulative SET range" << std::right
              << std::setw(kColWidth) << "Samples" << std::setw(kColWidth) << "Avg (us)"
              << std::setw(kColWidth) << "Median (us)" << std::setw(kColWidth) << "p90 (us)"
              << std::setw(kColWidth) << "p95 (us)" << std::setw(kColWidth) << "p99 (us)"
              << std::setw(kColWidth) << "p99.9 (us)" << std::setw(kColWidth) << "p99.99 (us)"
              << std::setw(kColWidth) << "Max (us)" << '\n';

    std::cout << std::string(kLabelWidth + kColWidth * 9, '-') << '\n';
}

// -----------------------------------------------------------------------------
// Workload operation selection
// -----------------------------------------------------------------------------

template <typename RNG>
Op choose_operation(RNG& rng, const std::vector<uint64_t>& known_key_ids) {
    std::discrete_distribution<int> distribution(kOpWeights.begin(), kOpWeights.end());

    Op op = static_cast<Op>(distribution(rng));

    // DEL requires a locally known live key. Fall back to GET (a point
    // lookup, likely a miss this early) rather than skipping the request.
    if (op == Op::DEL && known_key_ids.empty()) {
        op = Op::GET;
    }

    return op;
}

// -----------------------------------------------------------------------------
// Build one request
// -----------------------------------------------------------------------------

// On success, request_key_id holds the key id the request targeted (the
// new id for SET, the chosen live id for DEL) so the caller can update its
// local bookkeeping without re-parsing the request string. GET leaves it
// unset since GET never mutates known_key_ids.
template <typename RNG>
bool build_request(Op& op, RNG& rng, uint64_t& next_client_key_id,
                   const std::vector<uint64_t>& known_key_ids, const std::string& value_payload,
                   std::string& request, uint64_t& request_key_id) {
    op = choose_operation(rng, known_key_ids);

    switch (op) {
        case Op::SET: {
            request_key_id = next_client_key_id++;

            request =
                "SET|bench_key_" + std::to_string(request_key_id) + "|" + value_payload + "\n";

            return true;
        }

        case Op::GET: {
            // Deliberately use a broad ID range to produce both hits and misses.
            std::uniform_int_distribution<uint64_t> dist(
                1, std::max<uint64_t>(1, next_client_key_id + 1000));

            request = "GET|bench_key_" + std::to_string(dist(rng)) + "\n";

            return true;
        }

        case Op::DEL: {
            std::uniform_int_distribution<size_t> dist(0, known_key_ids.size() - 1);

            request_key_id = known_key_ids[dist(rng)];

            request = "DEL|bench_key_" + std::to_string(request_key_id) + "\n";

            return true;
        }
    }

    return false;
}

// -----------------------------------------------------------------------------
// Run one timed request
// -----------------------------------------------------------------------------

bool run_one_request(int fd, Op& op, std::mt19937_64& rng, uint64_t& next_client_key_id,
                     std::vector<uint64_t>& known_key_ids, const std::string& value_payload,
                     double& out_micros) {
    std::string request;
    uint64_t request_key_id = 0;

    if (!build_request(op, rng, next_client_key_id, known_key_ids, value_payload, request,
                       request_key_id)) {
        return false;
    }

    // Request generation is outside the timed region.
    //
    // Measured interval:
    //
    // send()
    //   -> TCP / kernel
    //   -> server receive / epoll / parsing / processing
    //   -> response generation / send()
    //   -> TCP / kernel
    //   -> complete newline-terminated response received
    const auto start = Clock::now();

    if (!send_all(fd, request)) {
        return false;
    }

    std::string response;

    if (!receive_line(fd, response)) {
        return false;
    }

    const auto end = Clock::now();

    out_micros = std::chrono::duration<double, std::micro>(end - start).count();

    // Update client-side state only after timing has completed.
    if (op == Op::SET) {
        if (response.rfind("OK", 0) == 0 && known_key_ids.size() < kMaxKnownKeys) {
            known_key_ids.push_back(request_key_id);
        }
    } else if (op == Op::DEL) {
        if (response.rfind("OK", 0) == 0) {
            auto it = std::find(known_key_ids.begin(), known_key_ids.end(), request_key_id);

            if (it != known_key_ids.end()) {
                known_key_ids.erase(it);
            }
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Worker thread
// -----------------------------------------------------------------------------

void worker_thread(const std::string& host, uint16_t port, int worker_index, uint64_t rng_seed,
                   uint64_t target, uint64_t progress_every, SharedState& shared,
                   std::array<std::vector<Sample>, 6>& out_buckets, int max_tier_index) {
    int fd = connect_to_server(host, port);

    if (fd < 0) {
        std::lock_guard<std::mutex> lock(shared.print_mutex);

        std::cerr << "[worker " << worker_index << "] failed to connect.\n";

        shared.failed.store(true, std::memory_order_relaxed);

        return;
    }

    uint64_t user_id = 0;

    if (!register_user(fd, "cold_bench_user_" + std::to_string(worker_index), user_id)) {
        std::lock_guard<std::mutex> lock(shared.print_mutex);

        std::cerr << "[worker " << worker_index << "] REGISTER failed.\n";

        shared.failed.store(true, std::memory_order_relaxed);

        close(fd);
        return;
    }

    std::mt19937_64 rng(rng_seed + static_cast<uint64_t>(worker_index));

    uint64_t next_client_key_id = 1;

    std::vector<uint64_t> known_key_ids;
    known_key_ids.reserve(256);

    const std::string value_payload(kValueSize, 'x');

    while (!shared.failed.load(std::memory_order_relaxed)) {
        const uint64_t accepted_now = shared.accepted_sets.load(std::memory_order_relaxed);

        if (accepted_now >= target) {
            break;
        }

        Op op = Op::GET;
        double latency_us = 0.0;

        if (!run_one_request(fd, op, rng, next_client_key_id, known_key_ids, value_payload,
                             latency_us)) {
            std::lock_guard<std::mutex> lock(shared.print_mutex);

            std::cerr << "[worker " << worker_index << "] request failed.\n";

            shared.failed.store(true, std::memory_order_relaxed);

            break;
        }

        uint64_t key_count_after = accepted_now;

        if (op == Op::SET) {
            key_count_after = shared.accepted_sets.fetch_add(1, std::memory_order_relaxed) + 1;
        } else {
            key_count_after = shared.accepted_sets.load(std::memory_order_relaxed);
        }

        int bucket = 0;

        while (bucket < max_tier_index && key_count_after > kTiers[bucket]) {
            ++bucket;
        }

        out_buckets[static_cast<size_t>(bucket)].push_back({latency_us});

        if (progress_every > 0) {
            uint64_t mark = shared.next_progress_mark.load(std::memory_order_relaxed);

            if (key_count_after >= mark) {
                if (shared.next_progress_mark.compare_exchange_strong(mark, mark + progress_every,
                                                                      std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> lock(shared.print_mutex);

                    std::cout << "  ...reached " << format_commas(key_count_after)
                              << " accepted SET requests\n";
                }
            }
        }
    }

    close(fd);
}

// -----------------------------------------------------------------------------
// Run cold-state benchmark
// -----------------------------------------------------------------------------

void run_cold_state(const std::string& host, uint16_t port, uint64_t rng_seed, int max_tier_index,
                    uint64_t progress_every, int num_connections) {
    std::cout << "\n=== Cold-state whole-server latency ===\n"
              << "Randomized mixed workload, " << num_connections << " concurrent connections, "
              << "one request at a time per connection\n"
              << "Key ranges are cumulative accepted SET requests, "
                 "not live key count (DELs are not subtracted).\n";

    const uint64_t target = kTiers[max_tier_index];

    SharedState shared;

    shared.next_progress_mark.store(
        progress_every > 0 ? progress_every : std::numeric_limits<uint64_t>::max(),
        std::memory_order_relaxed);

    std::vector<std::array<std::vector<Sample>, 6>> per_thread_buckets(
        static_cast<size_t>(num_connections));

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_connections));

    const auto wall_start = Clock::now();

    for (int i = 0; i < num_connections; ++i) {
        threads.emplace_back(worker_thread, host, port, i, rng_seed, target, progress_every,
                             std::ref(shared), std::ref(per_thread_buckets[static_cast<size_t>(i)]),
                             max_tier_index);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto wall_end = Clock::now();

    std::array<std::vector<Sample>, 6> merged;

    for (auto& worker_buckets : per_thread_buckets) {
        for (int i = 0; i <= max_tier_index; ++i) {
            auto& dst = merged[static_cast<size_t>(i)];

            auto& src = worker_buckets[static_cast<size_t>(i)];

            dst.insert(dst.end(), src.begin(), src.end());
        }
    }

    if (shared.failed.load(std::memory_order_relaxed)) {
        std::cerr << "One or more workers failed; "
                     "results below are partial.\n";
    }

    print_table_header("Cold-state latency (mixed SET/GET/DEL workload over TCP)");

    for (int i = 0; i <= max_tier_index; ++i) {
        const uint64_t lo = (i == 0) ? 0 : kTiers[i - 1] + 1;

        print_latency_row(range_label(lo, kTiers[i]), merged[static_cast<size_t>(i)]);
    }

    const double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();

    uint64_t total_samples = 0;

    for (int i = 0; i <= max_tier_index; ++i) {
        total_samples += merged[static_cast<size_t>(i)].size();
    }

    std::cout << "\nWall clock: " << std::fixed << std::setprecision(1) << wall_seconds
              << "s, measured requests: " << format_commas(total_samples) << " ("
              << format_commas(static_cast<uint64_t>(total_samples / std::max(wall_seconds, 0.001)))
              << " req/s)\n";
}

// -----------------------------------------------------------------------------
// Benchmark configuration
// -----------------------------------------------------------------------------

const std::string host = "127.0.0.1";
constexpr uint16_t port = 8080;

constexpr int max_tier_index = 4;
constexpr uint64_t rng_seed = 42;
constexpr uint64_t progress_every = 100000;
constexpr int num_connections = 8;

// -----------------------------------------------------------------------------
// Workload validation
// -----------------------------------------------------------------------------

bool validate_workload(const std::array<double, 3>& weights) {
    constexpr double kExpectedTotal = 100.0;
    constexpr double kEpsilon = 1e-9;

    double total = 0.0;

    for (double weight : weights) {
        if (weight < 0.0) {
            std::cerr << "ERROR: workload weight cannot "
                         "be negative.\n";

            return false;
        }

        total += weight;
    }

    if (std::abs(total - kExpectedTotal) > kEpsilon) {
        std::cerr << "ERROR: workload weights must sum "
                     "to 100%. Current total = "
                  << total << "%\n";

        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Print benchmark configuration
// -----------------------------------------------------------------------------

void print_benchmark_configuration() {
    std::cout << "\n==================================================\n"
              << "RUNNING COLD-STATE LATENCY BENCHMARK\n"
              << "==================================================\n";

    std::cout << "Target: " << host << ':' << port << '\n';

    std::cout << "Tiers: ";

    for (int i = 0; i <= max_tier_index; ++i) {
        std::cout << format_commas(kTiers[i]) << (i < max_tier_index ? ", " : "\n");
    }

    std::cout << "Connections: " << num_connections << '\n';

    std::cout << "RNG seed: " << rng_seed << '\n';

    std::cout << "Value size: " << kValueSize << " bytes\n";

    std::cout << "Progress every: " << format_commas(progress_every) << " accepted SET requests\n";

    std::cout << "\nMeasurement workload weights:\n"
              << "  SET = " << kOpWeights[0] << "%\n"
              << "  GET = " << kOpWeights[1] << "%\n"
              << "  DEL = " << kOpWeights[2] << "%\n"
              << "  TOTAL = 100%\n";
}

// -----------------------------------------------------------------------------
// Run one benchmark configuration
// -----------------------------------------------------------------------------

void run_benchmark(const std::array<double, 3>& weights) {
    kOpWeights = weights;

    if (!validate_workload(kOpWeights)) {
        return;
    }

    print_benchmark_configuration();

    run_cold_state(host, port, rng_seed, max_tier_index, progress_every, num_connections);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Default: 40% SET, 45% GET, 15% DEL — write-heavy to grow the keyspace
    // fast while still exercising the read and delete paths.
    std::array<double, 3> weights = {40, 45, 15};

    if (argc != 1 && argc != 4) {
        std::cerr << "Usage: " << argv[0] << " [SET GET DEL]\n"
                  << "Provide either no percentages "
                     "(uses defaults) or all 3 percentages.\n";

        return 1;
    }

    if (argc == 4) {
        try {
            for (int i = 0; i < 3; ++i) {
                weights[i] = std::stod(argv[i + 1]);
            }
        } catch (const std::exception&) {
            std::cerr << "Error: all workload percentages "
                         "must be valid numbers.\n";

            return 1;
        }
    }

    run_benchmark(weights);
}