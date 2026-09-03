// benchmarks/bench_net_endurance.cpp
// -----------------------------------------------------------------------------
// SCOPE: A 10-minute (600s) soak test against a live server on loopback.
// Runs 100 persistent TCP connections continuously issuing ~10% SET / 90% GET
// ops, reconnecting automatically if a connection drops.
//
// What to watch in the real-time telemetry:
//   - RPS trending downward over time      →  likely memory leak or CPU throttle.
//   - RPS hitting zero and staying there   →  deadlock.
//   - Rising failure count                 →  connection instability or server error.
//
// Like bench_net_throughput, numbers here are loopback-only. Absolute RPS
// will not translate to real-network deployments.
// -----------------------------------------------------------------------------

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

// CONFIGURATION
const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8080;
const int TEST_DURATION_SEC = 600;
const int THREADS = 100;                     // 100 Concurrent Connections
const int MINUTES = TEST_DURATION_SEC / 60;  // minutes

std::atomic<long long> total_success(0);
std::atomic<long long> total_fail(0);
std::atomic<bool> keep_running(true);

bool send_all(int sock, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(sock, data.c_str() + total_sent, data.size() - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

uint64_t register_shared_user(const std::string& username, const std::string& password) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

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

// Every reconnect logs in as the SAME shared user rather than registering a
// fresh one. Two reasons: (1) keys are namespaced per-user, so a fresh
// REGISTER on reconnect would silently lose access to everything written
// before the drop; (2) a 10-minute soak with periodic reconnects would grow
// UserBook unbounded under REGISTER, which would itself confound the
// leak-detection this test exists to do.
void worker_thread(int thread_id, uint64_t shared_user_id, std::string shared_password) {
    long long local_count = 0;

    // Outer loop: Reconnects if the connection dies
    while (keep_running) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Set 2-second timeout on socket operations
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::string login_cmd =
            "LOGIN|" + std::to_string(shared_user_id) + "|" + shared_password + "\n";
        if (!send_all(sock, login_cmd)) {
            close(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        char login_buf[256];
        ssize_t login_n = recv(sock, login_buf, sizeof(login_buf) - 1, 0);
        if (login_n <= 0) {
            close(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char buffer[1024];

        // Inner loop: issue SET/GET commands until the connection drops or test ends
        while (keep_running) {
            long long last_set = local_count - (local_count % 10);
            std::string command;
            if (local_count % 10 == 0) {
                command = "SET|key_" + std::to_string(thread_id) + "_" +
                          std::to_string(local_count) + "|value_" + std::to_string(local_count) +
                          "\n";
            } else {
                command =
                    "GET|key_" + std::to_string(thread_id) + "_" + std::to_string(last_set) + "\n";
            }

            if (send(sock, command.c_str(), command.size(), 0) < 0) {
                total_fail++;
                break;  // Break inner loop, outer loop will reconnect
            }
            ssize_t bytes_recv = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes_recv <= 0) {
                total_fail++;
                break;  // Break inner loop, outer loop will reconnect
            }

            // Validate the data hasn't been corrupted
            if (local_count % 10 != 0) {
                buffer[bytes_recv] = '\0';
                std::string expected = "value_" + std::to_string(last_set) + "\n";
                if (std::string(buffer) != expected) {
                    // Data corruption: response didn't match — counted as a failure, not a crash.
                    total_fail++;
                }
            }

            // Batch atomic updates every 1000 ops to avoid cache-coherency traffic
            // from a hot global counter dragging down the measurement itself.
            local_count++;
            if (local_count % 1000 == 0) {
                total_success += 1000;
            }
        }
        close(sock);
    }
}

void monitor_thread() {
    long long previous_count = 0;

    std::cout << "\n============================================================\n";
    std::cout << "         TEST: ENDURANCE TELEMETRY (" << MINUTES << " MIN)                 \n";
    std::cout << "============================================================\n";
    std::cout << "Target:       " << SERVER_IP << ":" << SERVER_PORT << "\n";
    std::cout << "Threads:      " << THREADS << "\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "| Minutes | Total Requests | Current RPS | Avg RPS |\n" << std::flush;

    for (int i = 1; i <= TEST_DURATION_SEC; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        long long current_count = total_success.load();
        long long current_rps = current_count - previous_count;
        previous_count = current_count;

        int mins = i / 60;
        int secs = i % 60;
        long long avg_rps = current_count / i;

        // Emit one telemetry row per second with a rolling RPS and running average.
        std::cout << "\r|  " << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2)
                  << secs << std::setfill(' ') << "  | " << std::setw(14) << current_count << " | "
                  << std::setw(11) << current_rps << " | " << std::setw(7) << avg_rps << " |"
                  << std::flush;
    }

    std::cout << "\n";
    keep_running = false;
    std::cout << "------------------------------------------------------------\n";
}

int main() {
    const std::string shared_password = "endurance_pw";
    uint64_t shared_user_id = register_shared_user("endurance_user", shared_password);
    if (shared_user_id == 0) {
        std::cerr << "Error: Could not register shared endurance user. Is the server running?\n";
        return 1;
    }

    std::vector<std::thread> threads;

    // Start Monitor
    std::thread monitor(monitor_thread);

    // Start Workers
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker_thread, i, shared_user_id, shared_password);
    }

    // Wait for Monitor to finish (after 600s)
    monitor.join();

    // Wait for workers to cleanup
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "                          RESULTS\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "Total Requests: " << total_success.load() << "\n";
    std::cout << "Failures:       " << total_fail.load() << "\n";
    std::cout << "============================================================\n\n\n";

    return 0;
}