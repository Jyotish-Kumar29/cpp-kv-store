#pragma once
#include <atomic>
#include <string>

#include "KVStore.hpp"
#include "UserBook.hpp"

enum class ConnState { AUTH, READY };

// Per-connection mutable state: I/O buffers, protocol phase, and
// authenticated user identity (0 until AUTH completes).
struct ClientState {
    std::string read_buf;
    std::string write_buf;
    ConnState state = ConnState::AUTH;
    uint64_t user_id = 0;
};

// Single-threaded epoll-based TCP server. Manages the full connection
// lifecycle: accept, auth, SET/GET/DEL dispatch, and graceful shutdown.
// stop() is safe to call from a signal handler or another thread.
class TCPServer {
private:
    int port;
    int server_fd;
    int epoll_fd;
    int wakeup_fd;  // eventfd used to interrupt the epoll wait loop on stop()
    KVStore store;

    UserBook user_book;

    // Per-connection protocol state and buffered I/O.
    std::unordered_map<int, ClientState> connections;

    std::atomic<bool> should_stop{false};

    void handle_auth_line(ClientState& cs, int fd, const std::string& line);
    void queue_write(int fd, ClientState& cs, const std::string& data);
    void close_connection(int fd);
    bool flush_write_buf(int fd, ClientState& cs);

public:
    // persistent is forwarded to KVStore — false disables AOF (used in tests).
    TCPServer(int port, bool persistent = true);
    ~TCPServer();

    void start();
    void stop();
    // Exposed for tests and embedding; called internally by start().
    int create_server_socket();

    // Dispatches one authenticated command without a network connection.
    // Used by integration tests to exercise command handling in-process.
    void process_request(std::string_view request, std::string& response, uint64_t user_id);

    // Test/embedding helper that registers a user without using
    // the network protocol.
    uint64_t register_user(const std::string& user_name, const std::string& password);
};
