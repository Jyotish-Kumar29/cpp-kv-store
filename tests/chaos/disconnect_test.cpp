#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <TCPServer.hpp>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

// Disconnect chaos tests: verifies that abrupt client disconnections at
// various protocol stages (before auth, mid-request, mid-stream) do not
// crash or corrupt the server, and that new connections are accepted normally
// afterwards.
class DisconnectTest : public ::testing::Test {
protected:
    TCPServer* server = nullptr;
    std::thread server_thread;
    int port = 18081;

    void SetUp() override {
        server = new TCPServer(port);

        server_thread = std::thread([this]() { server->start(); });

        // Allow the server thread to initialize before connecting.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    void TearDown() override {
        if (server != nullptr) {
            server->stop();

            if (server_thread.joinable()) {
                server_thread.join();
            }

            delete server;
            server = nullptr;
        }
    }

    int connect_to_server() {
        int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

        if (socket_fd < 0) {
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
            close(socket_fd);
            return -1;
        }

        if (connect(socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd);
            return -1;
        }

        return socket_fd;
    }
};

TEST_F(DisconnectTest, ImmediateDisconnect) {
    int socket_fd = connect_to_server();
    ASSERT_GE(socket_fd, 0);

    close(socket_fd);

    // Allow the server to observe and clean up the disconnected client.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    int socket_fd_2 = connect_to_server();

    EXPECT_GE(socket_fd_2, 0);

    if (socket_fd_2 >= 0) {
        close(socket_fd_2);
    }
}

TEST_F(DisconnectTest, DisconnectAfterPartialRequest) {
    int socket_fd = connect_to_server();
    ASSERT_GE(socket_fd, 0);

    send(socket_fd, "REGISTER|user", 13, 0);  // no '\n' — intentionally incomplete line

    close(socket_fd);

    // The partial request must not leave the server in a broken state.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    int socket_fd_2 = connect_to_server();

    EXPECT_GE(socket_fd_2, 0);

    if (socket_fd_2 >= 0) {
        close(socket_fd_2);
    }
}

TEST_F(DisconnectTest, DisconnectMidStream) {
    int socket_fd = connect_to_server();

    ASSERT_GE(socket_fd, 0) << "Failed to connect first socket";

    // Complete registration so the connection reaches READY state.
    const std::string register_command = "REGISTER|testuser1|pass\n";

    send(socket_fd, register_command.c_str(), register_command.size(), 0);

    char buffer[1024];

    int bytes = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);

    ASSERT_GT(bytes, 0) << "Failed to receive registration response";

    // Leave an incomplete request buffered before disconnecting.
    const std::string partial = "SET|hey";
    send(socket_fd, partial.c_str(), partial.size(), 0);

    close(socket_fd);

    // Allow the server to process the disconnect.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // A partially received request must not affect future connections.
    int socket_fd_2 = connect_to_server();

    ASSERT_GE(socket_fd_2, 0) << "Failed to connect second socket after mid-stream disconnect";

    timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    // Set a receive timeout so the test fails fast rather than hanging
    // if the server doesn't respond after the mid-stream disconnect.
    setsockopt(socket_fd_2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const std::string register_command_2 = "REGISTER|testuser2|pass\n";

    send(socket_fd_2, register_command_2.c_str(), register_command_2.size(), 0);

    bytes = recv(socket_fd_2, buffer, sizeof(buffer) - 1, 0);

    EXPECT_GT(bytes, 0) << "Server did not respond to new registration";

    if (bytes > 0) {
        buffer[bytes] = '\0';

        std::string response(buffer);

        EXPECT_TRUE(response.rfind("REGISTER_OK|", 0) == 0) << "Got: " << response;
    }

    close(socket_fd_2);
}

TEST_F(DisconnectTest, ManyRapidConnectionsAndDisconnections) {
    for (int i = 0; i < 100; ++i) {
        int socket_fd = connect_to_server();

        if (socket_fd >= 0) {
            close(socket_fd);
        }
    }

    // Give the event loop enough time to process the burst, especially
    // under instrumented builds such as ThreadSanitizer.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int socket_fd = connect_to_server();

    EXPECT_GE(socket_fd, 0);

    if (socket_fd >= 0) {
        close(socket_fd);
    }
}