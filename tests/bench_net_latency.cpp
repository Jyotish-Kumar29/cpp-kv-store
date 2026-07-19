// tests/benchmark_latency.cpp
// -----------------------------------------------------------------------------
// Precision Latency Analyzer with Worst-Case Lock Contention Scenarios
// -----------------------------------------------------------------------------

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8080;
const int NUM_REQUESTS = 50000;
const int NUM_STRESS_THREADS = 10;

// Helper to send all data over the socket
bool send_all(int sock, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(sock, data.c_str() + total_sent, data.size() - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

// Helper to connect to the server
int connect_to_server(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

// Background thread function to simulate heavy write/read load and lock contention
void stress_worker(int thread_id, const char* ip, int port, std::atomic<bool>& keep_running, int payload_size) {
    int sock = connect_to_server(ip, port);
    if (sock < 0) return;

    std::string large_val(payload_size, 'B');
    char buffer[1024];
    long long count = 0;

    while (keep_running.load(std::memory_order_relaxed)) {
        std::string command;
        if (thread_id % 2 == 0) {
            // Write payload - holds exclusive lock in kvstore
            command = "SET stress_key_" + std::to_string(thread_id) + "_" + std::to_string(count % 50) + " " + large_val + "\n";
        } else {
            // Read payload - holds shared lock in kvstore
            int target_thread = thread_id - 1;
            command = "GET stress_key_" + std::to_string(target_thread) + "_" + std::to_string(count % 50) + "\n";
        }
        count++;

        if (!send_all(sock, command)) {
            break;
        }

        if (thread_id % 2 == 0) {
            // Expecting "OK\n"
            ssize_t bytes_recv = recv(sock, buffer, sizeof(buffer), 0);
            if (bytes_recv <= 0) break;
        } else {
            // Expecting large value + \n. Read until we find '\n' at the end of the buffer stream
            std::string resp;
            while (keep_running.load(std::memory_order_relaxed)) {
                ssize_t bytes_recv = recv(sock, buffer, sizeof(buffer) - 1, 0);
                if (bytes_recv <= 0) break;
                resp.append(buffer, bytes_recv);
                if (!resp.empty() && resp.back() == '\n') {
                    break;
                }
            }
        }
    }
    close(sock);
}

// Run latency measurement loop
std::vector<double> measure_latency(int sock, int num_requests) {
    std::vector<double> latencies;
    latencies.reserve(num_requests);
    char buffer[1024];

    for (int i = 0; i < num_requests; ++i) {
        std::string command = "GET latency_test_key\n";

        auto start = std::chrono::high_resolution_clock::now();
        if (!send_all(sock, command)) {
            std::cerr << "Send failed in measurement.\n";
            break;
        }

        // Response for GET latency_test_key should be "test_val\n" (small)
        ssize_t bytes_recv = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes_recv <= 0) {
            std::cerr << "Recv failed in measurement.\n";
            break;
        }
        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::micro> elapsed = end - start;
        latencies.push_back(elapsed.count());
    }
    return latencies;
}

// Compute and print percentiles
void print_stats(const std::string& scenario_name, std::vector<double>& latencies) {
    if (latencies.empty()) {
        std::cout << "Error: No samples collected for " << scenario_name << "\n";
        return;
    }
    std::sort(latencies.begin(), latencies.end());

    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avg = sum / latencies.size();

    int n = latencies.size();
    double p50 = latencies[n * 0.50];
    double p95 = latencies[n * 0.95];
    double p99 = latencies[n * 0.99];
    double p99_9 = latencies[n * 0.999];

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "------------------------------------------------------------\n";
    std::cout << "   RESULTS: " << scenario_name << "\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "Average:      " << avg << " us\n";
    std::cout << "Min:          " << latencies.front() << " us\n";
    std::cout << "Max:          " << latencies.back() << " us\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "p50 (Median): " << p50 << " us\n";
    std::cout << "p95:          " << p95 << " us\n";
    std::cout << "p99 (Tail):   " << p99 << " us\n";
    std::cout << "p99.9:        " << p99_9 << " us\n";
    std::cout << "============================================================\n\n";
}

int main() {
    std::cout << "\n============================================================\n";
    std::cout << "         TEST: PRECISION LATENCY ANALYSIS WITH CONTENTION    \n";
    std::cout << "============================================================\n";
    std::cout << "Target:       " << SERVER_IP << ":" << SERVER_PORT << "\n";
    std::cout << "Samples:      " << NUM_REQUESTS << " per scenario\n";
    std::cout << "Warmup:       100 Requests\n";
    std::cout << "============================================================\n\n";

    int main_sock = connect_to_server(SERVER_IP, SERVER_PORT);
    if (main_sock < 0) {
        std::cerr << "Error: Could not connect to KV-Store server. Is it running?\n";
        return 1;
    }

    // Set up latency test key
    std::string setup_cmd = "SET latency_test_key test_val\n";
    send_all(main_sock, setup_cmd);
    char setup_buf[128];
    recv(main_sock, setup_buf, sizeof(setup_buf), 0);

    // Warm up the main connection
    for (int i = 0; i < 100; i++) {
        std::string cmd = "GET latency_test_key\n";
        send_all(main_sock, cmd);
        char buf[128];
        recv(main_sock, buf, sizeof(buf), 0);
    }

    // SCENARIO 1: BASELINE (UNCONTENDED)
    std::cout << "[Scenario 1] Running Baseline Latency Test...\n";
    auto baseline_latencies = measure_latency(main_sock, NUM_REQUESTS);
    print_stats("Scenario 1: Baseline (Uncontended)", baseline_latencies);

    // SCENARIO 2: CONTENDED (10 KB payloads)
    std::cout << "[Scenario 2] Spawning " << NUM_STRESS_THREADS << " stress workers with 10 KB payloads...\n";
    std::atomic<bool> keep_running_s2(true);
    std::vector<std::thread> threads_s2;
    for (int i = 0; i < NUM_STRESS_THREADS; ++i) {
        threads_s2.emplace_back(stress_worker, i, SERVER_IP, SERVER_PORT, std::ref(keep_running_s2), 10240);
    }

    // Allow background workers to start making requests
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "Measuring latency under contention (10 KB)..." << std::endl;
    auto contended_s2_latencies = measure_latency(main_sock, NUM_REQUESTS);

    keep_running_s2 = false;
    for (auto& t : threads_s2) {
        t.join();
    }

    print_stats("Scenario 2: Contended (10 KB payload lock contention)", contended_s2_latencies);

    // SCENARIO 3: CONTENDED (50 KB payloads)
    std::cout << "[Scenario 3] Spawning " << NUM_STRESS_THREADS << " stress workers with 50 KB payloads...\n";
    std::atomic<bool> keep_running_s3(true);
    std::vector<std::thread> threads_s3;
    for (int i = 0; i < NUM_STRESS_THREADS; ++i) {
        threads_s3.emplace_back(stress_worker, i, SERVER_IP, SERVER_PORT, std::ref(keep_running_s3), 51200);
    }

    // Allow background workers to start making requests
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "Measuring latency under contention (50 KB)..." << std::endl;
    auto contended_s3_latencies = measure_latency(main_sock, NUM_REQUESTS);

    keep_running_s3 = false;
    for (auto& t : threads_s3) {
        t.join();
    }

    print_stats("Scenario 3: Contended (50 KB payload lock contention)", contended_s3_latencies);

    close(main_sock);
    return 0;
}