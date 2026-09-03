// benchmarks/bench_net_throughput.cpp
// -----------------------------------------------------------------------------
// SCOPE: Measures request processing throughput under synthetic, zero-latency
// loopback conditions (127.0.0.1, 100 persistent connections, 1M total ops).
// The resulting req/sec figure reflects peak server capacity in an ideal
// environment — not real-world "network throughput." Across an actual network,
// expect significantly lower numbers due to round-trip latency, per-connection
// setup cost, and bandwidth constraints.
//
// std::memory_order_relaxed is used for atomic counters so that cache-coherency
// traffic from the load generator itself does not skew the measured rate.
// -----------------------------------------------------------------------------

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8080;
const int TOTAL_REQUESTS = 1000000;
const int THREADS = 100;

std::atomic<int> success_count(0);
std::atomic<int> fail_count(0);
std::atomic<int> last_error(0);

bool send_all(int sock, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(sock, data.c_str() + total_sent, data.size() - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

// Registers one shared user up front on a throwaway connection. Every worker
// thread logs in as this same user so SET/GET against the same literal key
// actually see each other's data (keys are namespaced per-user server-side).
uint64_t register_shared_user(const char* ip, int port, const std::string& username,
                              const std::string& password) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return 0;
    }

    std::string cmd = "REGISTER|" + username + "|" + password + "\n";
    if (!send_all(sock, cmd)) {
        close(sock);
        return 0;
    }

    char buf[256];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);
    if (n <= 0) return 0;
    buf[n] = '\0';

    std::string resp(buf);
    if (resp.rfind("REGISTER_OK|", 0) != 0) return 0;

    return std::stoull(resp.substr(std::string("REGISTER_OK|").size()));
}

void worker_thread(int requests_per_thread, uint64_t shared_user_id, std::string shared_password) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        last_error.store(errno, std::memory_order_relaxed);
        fail_count.fetch_add(requests_per_thread, std::memory_order_relaxed);
        return;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        last_error.store(errno, std::memory_order_relaxed);
        fail_count.fetch_add(requests_per_thread, std::memory_order_relaxed);
        close(sock);
        return;
    }

    std::string login_cmd =
        "LOGIN|" + std::to_string(shared_user_id) + "|" + shared_password + "\n";
    if (!send_all(sock, login_cmd)) {
        fail_count.fetch_add(requests_per_thread, std::memory_order_relaxed);
        close(sock);
        return;
    }
    char login_buf[256];
    ssize_t login_n = recv(sock, login_buf, sizeof(login_buf) - 1, 0);
    if (login_n <= 0) {
        fail_count.fetch_add(requests_per_thread, std::memory_order_relaxed);
        close(sock);
        return;
    }

    char buffer[1024];

    for (int i = 0; i < requests_per_thread; ++i) {
        std::string command;
        // ~10% SETs, ~90% GETs — intentionally read-heavy to stress the hot path
        if (rand() % 10 == 0) {
            command = "SET|key_" + std::to_string(i) + "|value_" + std::to_string(i) + "\n";
        } else {
            // Retrieve a key that was likely already set by this thread
            int key_idx = (i == 0) ? 0 : rand() % i;
            command = "GET|key_" + std::to_string(key_idx) + "\n";
        }

        if (send(sock, command.c_str(), command.size(), 0) < 0) {
            last_error.store(errno, std::memory_order_relaxed);
            fail_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        ssize_t bytes_received = recv(sock, buffer, sizeof(buffer), 0);

        if (bytes_received <= 0) {
            last_error.store(bytes_received == 0 ? ECONNRESET : errno, std::memory_order_relaxed);
            fail_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // Use relaxed memory order for high-performance lock-free counting
        success_count.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

int main() {
    std::cout << "\n============================================================\n";
    std::cout << "         TEST: THROUGHPUT BENCHMARK (STRESS)                \n";
    std::cout << "============================================================\n";
    std::cout << "Mode:         Persistent Connections (Keep-Alive)\n";
    std::cout << "Requests:     " << TOTAL_REQUESTS << "\n";
    std::cout << "Threads:      " << THREADS << "\n";

    const std::string shared_password = "bench_pw";
    uint64_t shared_user_id =
        register_shared_user(SERVER_IP, SERVER_PORT, "throughput_bench_user", shared_password);
    if (shared_user_id == 0) {
        std::cerr << "Error: Could not register shared bench user. Is the server running?\n";
        return 1;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    int req_per_thread = TOTAL_REQUESTS / THREADS;

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker_thread, req_per_thread, shared_user_id, shared_password);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end_time - start_time;

    // Read the relaxed atomics safely at the end of execution
    int final_success = success_count.load(std::memory_order_relaxed);
    int final_fail = fail_count.load(std::memory_order_relaxed);
    // req/sec under loopback — divide by ~10–100x mentally for real network RTT
    double throughput = final_success / duration.count();

    std::cout << "------------------------------------------------------------\n";
    std::cout << "                         RESULTS\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "Time:         " << duration.count() << " seconds\n";
    std::cout << "Throughput:   " << throughput << " req/sec\n";
    std::cout << "Failures:     " << final_fail << "\n";

    if (final_fail > 0) {
        std::cout << "------------------------------------------------------------\n";
        std::cout << "DIAGNOSTIC: Last System Error was '" << strerror(last_error.load()) << "'\n";
    }
    std::cout << "============================================================\n\n";

    return 0;
}