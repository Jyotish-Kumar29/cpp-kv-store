// tests/test_endurance.cpp
// -----------------------------------------------------------------------------
// GOAL: Verify Long-Term Stability (Memory Leaks, Deadlocks, Thrashing).
// METHODOLOGY:
//   1. Run for a sustained period (e.g., 10 minutes).
//   2. Use 100 concurrent threads to ensure race conditions trigger if present.
//   3. Monitor "Requests Per Second" in real-time.
//      - If RPS drops over time -> Possible Memory Leak or CPU Throttle.
//      - If RPS hits zero -> Deadlock.
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

void worker_thread(int thread_id) {
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

        char buffer[1024];

        // Inner loop: The actual work
        while (keep_running) {
            long long last_set = local_count - (local_count % 10);
            std::string command;
            if (local_count % 10 == 0) {
                command = "SET key_" + std::to_string(thread_id) + "_" +
                          std::to_string(local_count) + " value_" + std::to_string(local_count) +
                          "\n";
            } else {
                command = "GET key_" + std::to_string(thread_id) + "_" +
                          std::to_string(last_set) + "\n";
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
                    // Silent data corruption!
                    total_fail++;
                }
            }

            // Optimization: Update the global atomic less frequently
            local_count++;
            if (local_count % 1000 == 0) {
                total_success += 1000;
            }
        }
        close(sock);
    }
}

#include <fstream>
// ... (keep your other includes and global variables)

void monitor_thread() {
    auto start_time = std::chrono::system_clock::now();
    long long previous_count = 0;

    // Open CSV file for writing
    std::ofstream csv_file("tests/results/endurance_metrics.csv");
    csv_file << "Seconds_Elapsed,Total_Requests,RPS\n";

    std::cout << "\n============================================================\n";
    std::cout << "         TEST: ENDURANCE TELEMETRY (" << MINUTES << " MIN)                 \n";
    std::cout << "============================================================\n";
    std::cout << "Target:       " << SERVER_IP << ":" << SERVER_PORT << "\n";
    std::cout << "Threads:      " << THREADS << "\n";
    std::cout << "Logging to:   endurance_metrics.csv\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "| Minutes | Total Requests | Current RPS | Avg RPS |\n" << std::flush;

    for (int i = 1; i <= TEST_DURATION_SEC; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        long long current_count = total_success.load();
        long long current_rps = current_count - previous_count;
        previous_count = current_count;

        // Write to CSV
        csv_file << i << "," << current_count << "," << current_rps << "\n";

        // Calculate minutes and seconds
        int mins = i / 60;
        int secs = i % 60;
        long long avg_rps = current_count / i;

        // Print to console every 60 seconds
        std::cout << "\r|  " << std::setfill('0') << std::setw(2) << mins << ":" << std::setw(2)
                  << secs << std::setfill(' ') << "  | " << std::setw(14) << current_count << " | "
                  << std::setw(11) << current_rps << " | " << std::setw(7) << avg_rps << " |"
                  << std::flush;
    }

    std::cout << "\n";
    keep_running = false;
    csv_file.close();
    std::cout << "------------------------------------------------------------\n";
}

int main() {
    std::vector<std::thread> threads;

    // Start Monitor
    std::thread monitor(monitor_thread);

    // Start Workers
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker_thread, i);
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