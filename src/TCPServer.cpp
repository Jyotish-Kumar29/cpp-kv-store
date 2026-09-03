#include "TCPServer.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <thread>

#include "Utils.hpp"

constexpr int MAX_EVENTS = 1024;
constexpr int BUFFER_SIZE = 1024;
constexpr size_t MAX_REQUEST_SIZE = 64 * 1024;

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int TCPServer::create_server_socket() {
    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;

    // SO_REUSEADDR allows immediate rebind after server restart,
    // avoiding "address already in use" on TIME_WAIT sockets.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

TCPServer::TCPServer(int port_, bool persistent_)
    : port(port_), server_fd(-1), epoll_fd(-1), wakeup_fd(-1), store(persistent_) {
    // EFD_NONBLOCK: reading the eventfd in the epoll loop won't block.
    // EFD_CLOEXEC: child processes don't inherit this fd.
    wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (wakeup_fd == -1) {
        throw std::runtime_error(std::string("eventfd failed: ") + strerror(errno));
    }
}

TCPServer::~TCPServer() {
    for (auto& [fd, cs] : connections) {
        close(fd);
    }

    connections.clear();

    if (wakeup_fd >= 0) {
        close(wakeup_fd);
        wakeup_fd = -1;
    }

    if (epoll_fd >= 0) {
        close(epoll_fd);
        epoll_fd = -1;
    }

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

uint64_t TCPServer::register_user(const std::string& user_name, const std::string& password) {
    return user_book.register_user(user_name, password);
}

void TCPServer::close_connection(int fd) {
    auto it = connections.find(fd);

    if (it == connections.end()) {
        return;
    }

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);

    close(fd);
    connections.erase(it);
}

void TCPServer::queue_write(int fd, ClientState& cs, const std::string& data) {
    cs.write_buf.append(data);

    if (!flush_write_buf(fd, cs)) {
        return;
    }

    epoll_event ev{};
    ev.data.fd = fd;

    // Keep EPOLLOUT disabled while the output buffer is empty. Once a
    // partial write occurs, EPOLLOUT is enabled so epoll wakes us when
    // the socket becomes writable again.
    ev.events = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLRDHUP;

    if (!cs.write_buf.empty()) {
        ev.events |= EPOLLOUT;
    }

    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

bool TCPServer::flush_write_buf(int fd, ClientState& cs) {
    while (!cs.write_buf.empty()) {
        ssize_t sent = send(fd, cs.write_buf.data(), cs.write_buf.size(), MSG_NOSIGNAL);

        if (sent == 0) {
            close_connection(fd);
            return false;
        }

        if (sent > 0) {
            cs.write_buf.erase(0, static_cast<size_t>(sent));
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        close_connection(fd);
        return false;
    }

    return true;
}

void TCPServer::handle_auth_line(ClientState& cs, int fd, const std::string& line) {
    std::string_view sv(line);
    std::string_view command = next_field(sv);

    if (command == "REGISTER") {
        std::string_view user_name = next_field(sv);

        std::string_view password = sv;

        if (user_name.empty() || password.empty()) {
            queue_write(fd, cs, "AUTH_REJECT|EMPTY_CREDENTIALS\n");
            return;
        }

        uint64_t user_id = user_book.register_user(std::string(user_name), std::string(password));

        cs.user_id = user_id;
        cs.state = ConnState::READY;

        queue_write(fd, cs, "REGISTER_OK|" + std::to_string(user_id) + "\n");

    } else if (command == "LOGIN") {
        std::string_view user_id_str = next_field(sv);

        std::string_view password = sv;

        uint64_t user_id = 0;
        std::string err;

        bool ok = parse_uint64(user_id_str, user_id, err) && user_book.exists(user_id) &&
                  user_book.authenticate_user(user_id, std::string(password));

        if (!ok) {
            queue_write(fd, cs, "AUTH_REJECT|INVALID_CREDENTIALS\n");

            close_connection(fd);
            return;
        }

        cs.user_id = user_id;
        cs.state = ConnState::READY;

        queue_write(fd, cs,
                    "AUTH_OK|" + std::to_string(user_id) + "\n" + "Hi " +
                        user_book.get_user_name(user_id) + "\n");

    } else {
        queue_write(fd, cs, "AUTH_REJECT|UNKNOWN_COMMAND\n");
    }
}

void TCPServer::start() {
    server_fd = create_server_socket();

    if (server_fd < 0) {
        std::cerr << "Failed to crate the server socket, aborting.\n";
        return;
    }

    epoll_fd = epoll_create1(0);

    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return;
    }

    epoll_event wakeup_ev{};
    wakeup_ev.events = EPOLLIN;
    wakeup_ev.data.fd = wakeup_fd;

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeup_fd, &wakeup_ev);

    epoll_event server_ev{};
    server_ev.events = EPOLLIN | EPOLLET;
    server_ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &server_ev) < 0) {
        perror("epoll_ctl: server_fd");
        close(server_fd);
        close(epoll_fd);
        return;
    }

    epoll_event events[MAX_EVENTS];
    char buffer[BUFFER_SIZE];

    std::cout << "Epoll server is listening on the port " << port << "\n";

    while (!should_stop.load()) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (nfds < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == wakeup_fd) {
                uint64_t val;
                read(wakeup_fd, &val, sizeof(val));  // drain the eventfd; value is ignored
                continue;
            }

            if (fd == server_fd) {
                // EPOLLET requires draining accept() until EAGAIN.

                while (true) {
                    sockaddr_in clientaddr{};
                    socklen_t addr_len = sizeof(clientaddr);

                    int client_fd =
                        accept(server_fd, reinterpret_cast<sockaddr*>(&clientaddr), &addr_len);

                    if (client_fd < 0) {
                        if (errno == EAGAIN | errno == EWOULDBLOCK) {
                            break;
                        }

                        perror("accept");
                        break;
                    }

                    set_nonblocking(client_fd);

                    connections.emplace(client_fd, ClientState{});

                    epoll_event client_ev{};
                    client_ev.events = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLRDHUP;
                    client_ev.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                        perror("epoll_ctl : client");
                        close(client_fd);
                    }
                }

                continue;
            }

            if (events[i].events & EPOLLOUT) {
                auto it = connections.find(fd);

                if (it != connections.end()) {
                    if (!flush_write_buf(fd, it->second)) {
                        continue;
                    }

                    if (it->second.write_buf.empty()) {
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLRDHUP;
                        ev.data.fd = fd;

                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
                    }
                }
            }

            if (events[i].events & EPOLLIN) {
                auto it = connections.find(fd);

                if (it == connections.end()) {
                    continue;
                }

                ClientState& cs = it->second;

                bool dead = false;
                bool eof = false;

                while (true) {
                    ssize_t bytes_read = read(fd, buffer, BUFFER_SIZE - 1);

                    if (bytes_read < 0) {
                        if (errno == EAGAIN | errno == EWOULDBLOCK) {
                            break;
                        }

                        close_connection(fd);
                        dead = true;
                        break;
                    }

                    if (bytes_read == 0) {
                        eof = true;
                        break;
                    }

                    cs.read_buf.append(buffer, static_cast<size_t>(bytes_read));

                    if (cs.read_buf.size() > MAX_REQUEST_SIZE) {
                        queue_write(fd, cs, "REQUEST IS TOO LONG\n");
                        close_connection(fd);
                        dead = true;
                        break;
                    }
                }

                if (dead) {
                    continue;
                }

                size_t pos;

                while ((pos = cs.read_buf.find('\n')) != std::string::npos) {
                    std::string line = cs.read_buf.substr(0, pos);

                    cs.read_buf.erase(0, pos + 1);

                    if (cs.state != ConnState::READY) {
                        handle_auth_line(cs, fd, line);
                    } else {
                        std::string response;

                        process_request(line, response, cs.user_id);

                        queue_write(fd, cs, response);
                    }

                    if (connections.find(fd) == connections.end()) {
                        break;  // connection was closed during processing (e.g. failed LOGIN)
                    }
                }

                // If the peer half-closed after sending a complete request,
                // process the buffered request(s) before closing the socket.
                if (eof && connections.find(fd) != connections.end()) {
                    close_connection(fd);
                    continue;
                }
            }

            // Handle hangups that arrived without EPOLLIN. This can happen
            // when the peer connects and immediately closes without sending data.
            if ((events[i].events & (EPOLLRDHUP | EPOLLHUP)) && !(events[i].events & EPOLLIN) &&
                connections.find(fd) != connections.end()) {
                close_connection(fd);
            }
        }
    }
}

void TCPServer::stop() {
    should_stop.store(true);

    if (wakeup_fd >= 0) {
        uint64_t one = 1;

        write(wakeup_fd, &one, sizeof(one));  // unblocks epoll_wait, which waits indefinitely
    }
}

void TCPServer::process_request(std::string_view request, std::string& response, uint64_t user_id) {
    std::string_view command = next_field(request);

    if (command == "SET") {
        std::string_view key = next_field(request);

        if (key.empty()) {
            response = "REJECT|INVALID_INPUT|EMPTY_KEY\n";
            return;
        }

        std::string_view value = next_field(request);

        if (value.empty()) {
            response = "REJECT|INVALID_INPUT|EMPTY_VALUE\n";
            return;
        }

        // Keys are namespaced per user by prepending the user_id and ASCII
        // unit separator (\x1F). This byte cannot appear in wire-protocol
        // keys, guaranteeing no collision between users with identical key names.
        std::string final_key = std::to_string(user_id) + '\x1F' + std::string(key);

        store.set(final_key, std::string(value), response, user_id);

    } else if (command == "GET") {
        std::string_view key = next_field(request);

        if (key.empty()) {
            response = "REJECT|INVALID_INPUT|EMPTY_KEY\n";
            return;
        }

        std::string final_key = std::to_string(user_id) + '\x1F' + std::string(key);
        store.get(final_key, response, user_id);
    } else if (command == "DEL") {
        std::string_view key = next_field(request);

        if (key.empty()) {
            response = "REJECT|INVALID_INPUT|EMPTY_KEY\n";
            return;
        }

        std::string final_key = std::to_string(user_id) + '\x1F' + std::string(key);

        store.del(final_key, response, user_id);
    } else {
        response = "ERROR|INVALID_COMMAND\n";
    }
}