// steady_server_latency.cpp
//
// Whole-server TCP latency benchmark in warm/steady-state mode.
//
// Unlike a single shared data structure, keys in this KV store are
// namespaced per authenticated user. There is no single "the keyspace" to
// pre-seed once and share across measurement connections. Instead, for
// each tier, every measurement connection registers its own user and grows
// its OWN keyspace to the target size before measurement begins -- so a
// tier of N means "each connection's namespace holds N live keys," and the
// total data resident on the server at that tier is roughly
// N * num_connections. This is called out explicitly so the numbers aren't
// misread as a single N-key store.
//
// Growth (SET requests that populate the keyspace) is excluded from
// latency measurements. After growth, the same randomized mixed SET/GET/DEL
// workload is run and each request is
// timed independently. The keyspace is allowed to evolve during
// measurement (DELs remove keys, SETs add new ones), so a tier represents a
// *starting* state of approximately N live keys, not a size that holds
// steady throughout.
//
// SCOPE: All measurements are over loopback (127.0.0.1). Latency figures
// reflect server-side processing time plus kernel round-trip only — not
// real-network conditions. Expect significantly higher numbers over LAN/WAN.
//
// Usage:
//   ./steady_server_latency [SET GET DEL]
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
// Response parsing
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

// Receives one complete newline-terminated response. The timer in the
// measurement path stops when '\n' is received, so the measured interval
// covers the complete client/server request-response path.
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
// Parsing
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

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------

double percentile(const std::vector<Sample>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }

    const size_t index = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));

    return sorted[index].latency_us;
}

struct Stats {
    double avg = 0.0;
    double median = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double p9999 = 0.0;
    double max = 0.0;
};

Stats compute_stats(std::vector<Sample>& values) {
    Stats stats;

    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end(),
              [](const Sample& a, const Sample& b) { return a.latency_us < b.latency_us; });

    for (const auto& sample : values) {
        stats.avg += sample.latency_us;
    }

    stats.avg /= static_cast<double>(values.size());

    stats.median = percentile(values, 0.50);
    stats.p90 = percentile(values, 0.90);
    stats.p95 = percentile(values, 0.95);
    stats.p99 = percentile(values, 0.99);
    stats.p999 = percentile(values, 0.999);
    stats.p9999 = percentile(values, 0.9999);
    stats.max = values.back().latency_us;

    return stats;
}

void print_latency_row(const std::string& label, std::vector<Sample>& values) {
    constexpr int kLabelWidth = 22;
    constexpr int kColWidth = 13;

    std::cout << std::left << std::setw(kLabelWidth) << label;

    if (values.empty()) {
        std::cout << "(no samples)\n";
        return;
    }

    Stats stats = compute_stats(values);

    std::cout << std::right << std::setw(kColWidth) << values.size() << std::fixed
              << std::setprecision(3) << std::setw(kColWidth) << stats.avg << std::setw(kColWidth)
              << stats.median << std::setw(kColWidth) << stats.p90 << std::setw(kColWidth)
              << stats.p95 << std::setw(kColWidth) << stats.p99 << std::setw(kColWidth)
              << stats.p999 << std::setw(kColWidth) << stats.p9999 << std::setw(kColWidth)
              << stats.max << '\n';
}

void print_header(const std::string& title, const std::string& first_column) {
    constexpr int kLabelWidth = 22;
    constexpr int kColWidth = 13;

    std::cout << '\n' << title << '\n';

    std::cout << std::left << std::setw(kLabelWidth) << first_column << std::right
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

    // DEL requires a locally known live key. This should be rare once the
    // keyspace has been pre-seeded, but fall back to GET rather than skip.
    if (op == Op::DEL && known_key_ids.empty()) {
        op = Op::GET;
    }

    return op;
}

// -----------------------------------------------------------------------------
// Build measurement request
// -----------------------------------------------------------------------------

// On success, request_key_id holds the key id the request targeted (new id
// for SET, chosen live id for DEL). Unset for GET, which never mutates
// known_key_ids.
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
            // Includes both likely hits (within the seeded/created range)
            // and misses (the +1000 buffer beyond it).
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
// Run one measured request
// -----------------------------------------------------------------------------

bool run_one_measurement(int fd, std::mt19937_64& rng, uint64_t& next_client_key_id,
                         std::vector<uint64_t>& known_key_ids, const std::string& value_payload,
                         double& out_micros) {
    Op op;
    std::string request;
    uint64_t request_key_id = 0;

    if (!build_request(op, rng, next_client_key_id, known_key_ids, value_payload, request,
                       request_key_id)) {
        return false;
    }

    // Request construction is intentionally excluded from the measurement.
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

    // Update local client state only after timing has completed.
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
// Grow this connection's own keyspace to the requested size
// -----------------------------------------------------------------------------

// Setup is deliberately separate from the measurement workload and excluded
// from timing. Keys are pipelined in batches (send a batch of SET lines,
// then read back that many newline-terminated responses) to keep setup
// fast at large tiers -- each key is still a distinct, previously-unset
// key, so every setup request is expected to succeed.
bool grow_local_keyspace(int fd, uint64_t count, uint64_t& next_client_key_id,
                         std::vector<uint64_t>& known_key_ids, const std::string& value_payload,
                         size_t batch_size) {
    std::string batch_request;
    std::string recv_buf;
    char io_buf[65536];

    uint64_t remaining = count;

    while (remaining > 0) {
        const size_t this_batch = static_cast<size_t>(std::min<uint64_t>(batch_size, remaining));

        batch_request.clear();
        batch_request.reserve(this_batch * (value_payload.size() + 32));

        std::vector<uint64_t> batch_ids;
        batch_ids.reserve(this_batch);

        for (size_t i = 0; i < this_batch; ++i) {
            const uint64_t key_id = next_client_key_id++;

            batch_ids.push_back(key_id);

            batch_request += "SET|bench_key_";
            batch_request += std::to_string(key_id);
            batch_request += "|";
            batch_request += value_payload;
            batch_request += "\n";
        }

        if (!send_all(fd, batch_request)) {
            return false;
        }

        size_t lines_seen = 0;

        while (lines_seen < this_batch) {
            size_t pos;

            while (lines_seen < this_batch && (pos = recv_buf.find('\n')) != std::string::npos) {
                std::string_view line(recv_buf.data(), pos);

                if (line.rfind("OK", 0) != 0) {
                    // Every setup key is distinct and previously unset, so
                    // anything other than OK means setup itself is broken --
                    // don't silently count a failed seed as live.
                    return false;
                }

                if (known_key_ids.size() < kMaxKnownKeys) {
                    known_key_ids.push_back(batch_ids[lines_seen]);
                }

                recv_buf.erase(0, pos + 1);

                ++lines_seen;
            }

            if (lines_seen < this_batch) {
                ssize_t received = recv(fd, io_buf, sizeof(io_buf), 0);

                if (received <= 0) {
                    return false;
                }

                recv_buf.append(io_buf, static_cast<size_t>(received));
            }
        }

        remaining -= this_batch;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Measure one tier
// -----------------------------------------------------------------------------

// Each measurement worker owns its own TCP connection, user, RNG, and
// pre-seeded keyspace. Warmup requests are excluded from the reported
// samples. Warmup requests are excluded from the reported samples.
bool measure_tier(const std::string& host, uint16_t port, uint64_t target_keys,
                  uint64_t samples_per_tier, uint64_t warmup_per_tier, uint64_t rng_seed,
                  int tier_index, int num_connections, size_t growth_batch_size,
                  std::vector<Sample>& output) {
    output.clear();
    output.reserve(static_cast<size_t>(samples_per_tier));

    std::vector<std::thread> workers;
    std::mutex output_mutex;
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> completed_samples{0};

    const int workers_count = std::max(1, num_connections);

    const uint64_t total_samples = samples_per_tier;

    const std::string value_payload(kValueSize, 'x');

    auto worker = [&](int worker_index) {
        int fd = connect_to_server(host, port);

        if (fd < 0) {
            failed.store(true, std::memory_order_relaxed);
            return;
        }

        uint64_t user_id = 0;

        if (!register_user(fd,
                           "steady_bench_user_" + std::to_string(tier_index) + "_" +
                               std::to_string(worker_index),
                           user_id)) {
            failed.store(true, std::memory_order_relaxed);

            close(fd);
            return;
        }

        std::mt19937_64 rng(rng_seed +
                            0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(tier_index + 1) +
                            static_cast<uint64_t>(worker_index));

        uint64_t next_client_key_id = 1;

        std::vector<uint64_t> known_key_ids;
        known_key_ids.reserve(std::min<size_t>(kMaxKnownKeys, static_cast<size_t>(target_keys)));

        // Grow this connection's own namespace to the target tier size.
        // Excluded from timing.
        if (!grow_local_keyspace(fd, target_keys, next_client_key_id, known_key_ids, value_payload,
                                 growth_batch_size)) {
            failed.store(true, std::memory_order_relaxed);

            close(fd);
            return;
        }

        // Warmup requests exercise the server before samples are collected.
        const uint64_t local_warmup =
            warmup_per_tier / static_cast<uint64_t>(workers_count) +
            (static_cast<uint64_t>(worker_index) <
                     warmup_per_tier % static_cast<uint64_t>(workers_count)
                 ? 1
                 : 0);

        for (uint64_t i = 0; i < local_warmup; ++i) {
            if (failed.load(std::memory_order_relaxed)) {
                break;
            }

            double ignored_latency = 0.0;

            if (!run_one_measurement(fd, rng, next_client_key_id, known_key_ids, value_payload,
                                     ignored_latency)) {
                failed.store(true, std::memory_order_relaxed);

                break;
            }
        }

        const uint64_t base = total_samples / static_cast<uint64_t>(workers_count);

        const uint64_t remainder = total_samples % static_cast<uint64_t>(workers_count);

        const uint64_t local_sample_count =
            base + (static_cast<uint64_t>(worker_index) < remainder ? 1 : 0);

        std::vector<Sample> local_samples;
        local_samples.reserve(static_cast<size_t>(local_sample_count));

        for (uint64_t i = 0; i < local_sample_count; ++i) {
            if (failed.load(std::memory_order_relaxed)) {
                break;
            }

            double latency_us = 0.0;

            if (!run_one_measurement(fd, rng, next_client_key_id, known_key_ids, value_payload,
                                     latency_us)) {
                failed.store(true, std::memory_order_relaxed);

                break;
            }

            local_samples.push_back({latency_us});

            completed_samples.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(output_mutex);

            output.insert(output.end(), local_samples.begin(), local_samples.end());
        }

        close(fd);
    };

    workers.reserve(static_cast<size_t>(workers_count));

    for (int i = 0; i < workers_count; ++i) {
        workers.emplace_back(worker, i);
    }

    for (auto& thread : workers) {
        thread.join();
    }

    if (failed.load(std::memory_order_relaxed) ||
        completed_samples.load(std::memory_order_relaxed) != total_samples) {
        std::cerr << "Measurement failed at tier " << format_commas(kTiers[tier_index])
                  << "; collected " << completed_samples.load() << " / " << total_samples
                  << " samples.\n";

        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Default benchmark configuration
// -----------------------------------------------------------------------------

const std::string host = "127.0.0.1";

constexpr uint16_t port = 8080;

constexpr uint64_t samples_per_tier = 5000;
constexpr uint64_t warmup_per_tier = 200;
constexpr uint64_t rng_seed = 42;

constexpr int max_tier_index = 4;

constexpr size_t growth_batch_size = 500;

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
              << "RUNNING WARM / STEADY-STATE LATENCY BENCHMARK\n"
              << "==================================================\n";

    std::cout << "Target: " << host << ':' << port << '\n';

    std::cout << "Per-connection keyspace sizes: ";

    for (int i = 0; i <= max_tier_index; ++i) {
        std::cout << format_commas(kTiers[i]) << (i < max_tier_index ? ", " : "\n");
    }

    std::cout << "Samples per tier: " << format_commas(samples_per_tier) << '\n';

    std::cout << "Warmup per tier: " << format_commas(warmup_per_tier) << '\n';

    std::cout << "Measurement connections: " << num_connections << " (each with its own "
              << "namespace -- total resident data per tier is roughly "
              << "tier_size x connections)\n";

    std::cout << "Value size: " << kValueSize << " bytes\n";

    std::cout << "\nMeasurement workload weights:\n"
              << "  SET = " << kOpWeights[0] << "%\n"
              << "  GET = " << kOpWeights[1] << "%\n"
              << "  DEL = " << kOpWeights[2] << "%\n"
              << "  TOTAL = 100%\n";
}

// -----------------------------------------------------------------------------
// Run one complete benchmark configuration
// -----------------------------------------------------------------------------

void run_benchmark(const std::array<double, 3>& weights) {
    kOpWeights = weights;

    if (!validate_workload(kOpWeights)) {
        return;
    }

    print_benchmark_configuration();

    std::vector<std::pair<std::string, std::vector<Sample>>> results;

    results.reserve(static_cast<size_t>(max_tier_index + 1));

    for (int tier = 0; tier <= max_tier_index; ++tier) {
        const uint64_t target = kTiers[tier];

        std::cout << "  Seeding each connection to " << format_commas(target)
                  << " live keys and measuring...\n";

        std::vector<Sample> samples;

        if (!measure_tier(host, port, target, samples_per_tier, warmup_per_tier, rng_seed, tier,
                          num_connections, growth_batch_size, samples)) {
            return;
        }

        results.emplace_back(format_commas(target), std::move(samples));
    }

    print_header(
        "Warm/steady-state latency "
        "(mixed SET/GET/DEL workload over TCP)",
        "Starting live keys");

    for (auto& [label, samples] : results) {
        print_latency_row(label, samples);
    }
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