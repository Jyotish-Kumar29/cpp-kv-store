//
// TCP client scalability/load generator for cpp-kv-store.
//
// Maintains a large number of real TCP connections to the KV store server
// using a single epoll instance. No thread is created per client.
//
// Each client:
//   1. establishes a TCP connection,
//   2. registers once with a unique username,
//   3. remains connected,
//   4. periodically sends a lightweight GET request for a non‑existent key.
//
// The test targets the server's network/session layer:
//   client sockets -> epoll -> ClientState -> protocol parsing -> response
//
// GET is used because it does not modify the store and does not trigger
// persistence (AOF) writes, so it isolates the network stack and session
// handling from heavy disk or map operations.
//
// Usage:
//   ./kv_scalability_test [host] [port] [clients] [requests_per_client/sec] [duration_sec]
//
// Example:
//   ./kv_scalability_test 127.0.0.1 8080 20000 10 30
//

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kMaxEvents = 4096;
constexpr size_t kReadBufferSize = 4096;
constexpr size_t kMaxResponseBuffer = 64 * 1024;
constexpr int kProgressEverySeconds = 5;

enum class State { CONNECTING, AUTHENTICATING, READY, FAILED };

struct Client {
    int fd = -1;
    uint64_t id = 0;
    State state = State::CONNECTING;
    std::string read_buf;
    std::string write_buf;
    uint64_t requests_sent = 0;
    uint64_t responses_received = 0;
};

bool raise_fd_limit(size_t required) {
    rlimit limit{};

    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return false;
    }

    if (limit.rlim_cur >= required) {
        return true;
    }

    const rlim_t target = std::min<rlim_t>(static_cast<rlim_t>(required), limit.rlim_max);

    if (target <= limit.rlim_cur) {
        return false;
    }

    limit.rlim_cur = target;

    return setrlimit(RLIMIT_NOFILE, &limit) == 0;
}

void close_client(int epoll_fd, Client& client) {
    if (client.fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client.fd, nullptr);
        close(client.fd);
        client.fd = -1;
    }
    client.state = State::FAILED;
}

bool send_all_nonblocking(Client& client) {
    while (!client.write_buf.empty()) {
        const ssize_t sent =
            send(client.fd, client.write_buf.data(), client.write_buf.size(), MSG_NOSIGNAL);

        if (sent > 0) {
            client.write_buf.erase(0, static_cast<size_t>(sent));
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }

        if (sent < 0 && errno == EINTR) {
            continue;
        }

        return false;
    }
    return true;
}

bool update_interest(int epoll_fd, Client& client) {
    epoll_event event{};
    event.data.ptr = &client;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;

    if (client.state == State::CONNECTING || !client.write_buf.empty()) {
        event.events |= EPOLLOUT;
    }

    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client.fd, &event) == 0;
}

bool queue_request(int epoll_fd, Client& client, std::string request) {
    client.write_buf += request;

    if (!send_all_nonblocking(client)) {
        return false;
    }

    return update_interest(epoll_fd, client);
}

bool handle_connect(int epoll_fd, Client& client) {
    int error = 0;
    socklen_t error_len = sizeof(error);

    if (getsockopt(client.fd, SOL_SOCKET, SO_ERROR, &error, &error_len) != 0) {
        return false;
    }

    if (error != 0) {
        errno = error;
        return false;
    }

    client.state = State::AUTHENTICATING;

    // Register with a unique username per client.
    const std::string username = "scale_client_" + std::to_string(client.id);
    const std::string request = "REGISTER|" + username + "|pw\n";

    return queue_request(epoll_fd, client, request);
}

bool handle_read(int epoll_fd, Client& client) {
    char buffer[kReadBufferSize];

    while (true) {
        const ssize_t bytes = recv(client.fd, buffer, sizeof(buffer), 0);

        if (bytes > 0) {
            client.read_buf.append(buffer, static_cast<size_t>(bytes));

            if (client.read_buf.size() > kMaxResponseBuffer) {
                return false;
            }
            continue;
        }

        if (bytes == 0) {
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        return false;
    }

    while (true) {
        const size_t newline_pos = client.read_buf.find('\n');

        if (newline_pos == std::string::npos) {
            break;
        }

        const std::string_view line(client.read_buf.data(), newline_pos);

        if (client.state == State::AUTHENTICATING) {
            // Expect "REGISTER_OK|<user_id>". Anything else is a failure.
            if (line.rfind("REGISTER_OK|", 0) != 0) {
                return false;
            }
            client.state = State::READY;
        } else if (client.state == State::READY) {
            // Each line is a response to a GET or other command.
            ++client.responses_received;
        }

        client.read_buf.erase(0, newline_pos + 1);
    }

    return update_interest(epoll_fd, client);
}

void send_periodic_request(int epoll_fd, Client& client) {
    if (client.state != State::READY) {
        return;
    }

    // Lightweight GET for a key that does not exist.
    const std::string request = "GET|benchmark_key\n";

    if (queue_request(epoll_fd, client, request)) {
        ++client.requests_sent;
    }
}

void print_progress(const std::vector<Client>& clients, uint64_t elapsed_seconds) {
    uint64_t connected = 0;
    uint64_t ready = 0;
    uint64_t authenticating = 0;
    uint64_t connecting = 0;
    uint64_t failed = 0;
    uint64_t sent = 0;
    uint64_t received = 0;

    for (const Client& client : clients) {
        switch (client.state) {
            case State::CONNECTING:
                ++connecting;
                break;
            case State::AUTHENTICATING:
                ++connected;
                ++authenticating;
                break;
            case State::READY:
                ++connected;
                ++ready;
                break;
            case State::FAILED:
                ++failed;
                break;
        }

        sent += client.requests_sent;
        received += client.responses_received;
    }

    std::cout << "[" << elapsed_seconds << "s]"
              << " connected=" << connected << " ready=" << ready
              << " authenticating=" << authenticating << " connecting=" << connecting
              << " failed=" << failed << " requests_sent=" << sent
              << " responses_received=" << received << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t port = argc > 2 ? static_cast<uint16_t>(std::stoul(argv[2])) : 8080;
    const size_t client_count = argc > 3 ? static_cast<size_t>(std::stoull(argv[3])) : 20000;
    const uint64_t requests_per_second = argc > 4 ? std::stoull(argv[4]) : 10;
    const uint64_t duration_seconds = argc > 5 ? std::stoull(argv[5]) : 60;

    if (client_count == 0 || requests_per_second == 0 || duration_seconds == 0) {
        std::cerr << "clients, requests/sec, and duration "
                     "must all be greater than zero\n";
        return 1;
    }

    // +1024 reserves headroom for epoll_fd, timer_fd, stdio, and other process fds.
    if (!raise_fd_limit(client_count + 1024)) {
        std::cerr << "WARNING: could not raise RLIMIT_NOFILE sufficiently\n";
    }

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    const int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        std::cerr << "timerfd_create failed: " << std::strerror(errno) << '\n';
        close(epoll_fd);
        return 1;
    }

    const uint64_t interval_ns = 1000000000ULL / requests_per_second;
    itimerspec timer_spec{};
    timer_spec.it_value.tv_sec = 1;   // 1-second delay before first tick — gives clients time to connect
    timer_spec.it_value.tv_nsec = 0;
    timer_spec.it_interval.tv_sec = interval_ns / 1000000000ULL;
    timer_spec.it_interval.tv_nsec = interval_ns % 1000000000ULL;

    if (timerfd_settime(timer_fd, 0, &timer_spec, nullptr) != 0) {
        std::cerr << "timerfd_settime failed: " << std::strerror(errno) << '\n';
        close(timer_fd);
        close(epoll_fd);
        return 1;
    }

    // Timer events use a null data.ptr. Client events use a Client*.
    epoll_event timer_event{};
    timer_event.events = EPOLLIN;
    timer_event.data.ptr = nullptr;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &timer_event) != 0) {
        std::cerr << "epoll_ctl(timer_fd) failed: " << std::strerror(errno) << '\n';
        close(timer_fd);
        close(epoll_fd);
        return 1;
    }

    std::vector<Client> clients(client_count);

    std::cout << "=== KV‑Store TCP Client Scalability Test ===\n"
              << "Target: " << host << ':' << port << '\n'
              << "Clients: " << client_count << '\n'
              << "Client request rate limit: " << requests_per_second << "req/s\n"
              << "Duration: " << duration_seconds << "s\n\n";

    const auto start = std::chrono::steady_clock::now();

    size_t next_client = 0;
    size_t active_clients = 0;

    // Create all requested client sockets and add them to epoll.
    while (next_client < client_count) {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            std::cerr << "socket() failed after " << next_client
                      << " clients: " << std::strerror(errno) << '\n';
            break;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            close(fd);
            std::cerr << "Invalid IPv4 address: " << host << '\n';
            break;
        }

        errno = 0;
        const int connect_result =
            connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));

        Client& client = clients[next_client];
        client.fd = fd;
        client.id = static_cast<uint64_t>(next_client);

        if (connect_result == 0) {
            client.state = State::AUTHENTICATING;
        } else if (errno == EINPROGRESS) {
            client.state = State::CONNECTING;
        } else {
            client.state = State::FAILED;
            close(fd);
            client.fd = -1;
            ++next_client;
            continue;
        }

        epoll_event event{};
        event.data.ptr = &client;
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;

        if (client.state == State::CONNECTING) {
            event.events |= EPOLLOUT;
        } else {
            // Already connected, send registration immediately.
            const std::string username = "scale_client_" + std::to_string(client.id);
            client.write_buf = "REGISTER|" + username + "|pw\n";
            event.events |= EPOLLOUT;
        }

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
            close(fd);
            client.fd = -1;
            client.state = State::FAILED;
        } else {
            ++active_clients;
        }

        ++next_client;

        if ((next_client % 1000) == 0) {
            std::cout << "  initiated " << next_client << " / " << client_count << " connections\n";
        }
    }

    std::vector<epoll_event> events(kMaxEvents);
    uint64_t next_progress_second = kProgressEverySeconds;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t elapsed_seconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - start).count());

        if (elapsed_seconds >= duration_seconds) {
            break;
        }

        // 250 ms timeout ensures elapsed-time and progress checks run even with no events.
        const int nfds = epoll_wait(epoll_fd, events.data(), kMaxEvents, 250);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            // Timer event
            if (events[i].data.ptr == nullptr) {
                uint64_t expirations = 0;
                if (read(timer_fd, &expirations, sizeof(expirations)) ==
                    static_cast<ssize_t>(sizeof(expirations))) {
                    for (Client& client : clients) {
                        if (client.state != State::READY) {
                            continue;
                        }
                        for (uint64_t n = 0; n < expirations; ++n) {
                            send_periodic_request(epoll_fd, client);
                        }
                    }
                }
                continue;
            }

            Client* client = static_cast<Client*>(events[i].data.ptr);
            if (client->fd < 0) {
                continue;
            }

            const uint32_t event_mask = events[i].events;

            if (client->state == State::CONNECTING &&
                (event_mask & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                if (!handle_connect(epoll_fd, *client)) {
                    close_client(epoll_fd, *client);
                    continue;
                }
            }

            if (event_mask & EPOLLIN) {
                if (!handle_read(epoll_fd, *client)) {
                    close_client(epoll_fd, *client);
                    continue;
                }
            }

            if (event_mask & EPOLLOUT) {
                if (!send_all_nonblocking(*client)) {
                    close_client(epoll_fd, *client);
                    continue;
                }
                if (!update_interest(epoll_fd, *client)) {
                    close_client(epoll_fd, *client);
                    continue;
                }
            }

            if (event_mask & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                if (!(event_mask & EPOLLIN)) {
                    close_client(epoll_fd, *client);
                }
            }
        }

        if (elapsed_seconds >= next_progress_second) {
            print_progress(clients, elapsed_seconds);
            while (next_progress_second <= elapsed_seconds) {
                next_progress_second += kProgressEverySeconds;
            }
        }
    }

    // Final statistics and cleanup.
    uint64_t ready_clients = 0;
    uint64_t authenticating_clients = 0;
    uint64_t connecting_clients = 0;
    uint64_t failed_clients = 0;
    uint64_t total_requests = 0;
    uint64_t total_responses = 0;

    for (Client& client : clients) {
        switch (client.state) {
            case State::CONNECTING:
                ++connecting_clients;
                break;
            case State::AUTHENTICATING:
                ++authenticating_clients;
                break;
            case State::READY:
                ++ready_clients;
                break;
            case State::FAILED:
                ++failed_clients;
                break;
        }
        total_requests += client.requests_sent;
        total_responses += client.responses_received;
        if (client.fd >= 0) {
            close_client(epoll_fd, client);
        }
    }

    close(timer_fd);
    close(epoll_fd);

    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - start).count();

    std::cout << "\n=== Results ===\n"
              << "Target clients: " << client_count << '\n'
              << "Client sockets created: " << active_clients << '\n'
              << "Clients ready: " << ready_clients << '\n'
              << "Clients authenticating: " << authenticating_clients << '\n'
              << "Clients connecting: " << connecting_clients << '\n'
              << "Failed clients: " << failed_clients << '\n'
              << "Requests sent: " << total_requests << '\n'
              << "Responses received: " << total_responses << '\n'
              << "Elapsed: " << elapsed << " s\n"
              << "Aggregate request rate: " << (total_requests / std::max(elapsed, 0.001))
              << " req/s\n"
              << "Aggregate response rate: " << (total_responses / std::max(elapsed, 0.001))
              << " resp/s\n";

    if (ready_clients != client_count || failed_clients != 0 || authenticating_clients != 0 ||
        connecting_clients != 0) {
        std::cerr << "RESULT: FAIL - not all clients remained usable\n";
        return 1;
    }

    std::cout << "RESULT: PASS\n";
    return 0;
}