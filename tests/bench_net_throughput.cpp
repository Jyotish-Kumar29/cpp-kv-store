// tests/benchmark_throughput.cpp
// -----------------------------------------------------------------------------
// INDUSTRY STANDARD UPGRADES:
// - Uses std::memory_order_relaxed for atomic counters to prevent CPU cache
//   coherency traffic from bottlenecking the load generator itself.
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

void worker_thread(int requests_per_thread) {
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

    char buffer[1024];

    for (int i = 0; i < requests_per_thread; ++i) {
        std::string command;
        if (rand() % 10 == 0) {
            command = "SET key_" + std::to_string(i) + " value_" + std::to_string(i) + "\n";
        } else {
            // Retrieve a key that was likely already set by this thread
            int key_idx = (i == 0) ? 0 : rand() % i;
            command = "GET key_" + std::to_string(key_idx) + "\n";
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

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    int req_per_thread = TOTAL_REQUESTS / THREADS;

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker_thread, req_per_thread);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end_time - start_time;

    // Read the relaxed atomics safely at the end of execution
    int final_success = success_count.load(std::memory_order_relaxed);
    int final_fail = fail_count.load(std::memory_order_relaxed);
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