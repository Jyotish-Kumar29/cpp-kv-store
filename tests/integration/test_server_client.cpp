#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <TCPServer.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

// Live TCP integration tests: spawns a real TCPServer on a background thread
// and connects via actual sockets. Tests the full request/response path over
// the wire, including auth, pipelining, and user isolation.
class ServerClientTest : public ::testing::Test {
protected:
    TCPServer* server = nullptr;
    std::thread server_thread;
    const int port = 18080;
    int sock = -1;

    void SetUp() override {
        server = new TCPServer(port, false);

        server_thread = std::thread([this]() { server->start(); });

        // Wait briefly for the server to create its listening socket.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        sock = connect_to_server();

        ASSERT_GE(sock, 0) << "Failed to connect to server";

        const std::string register_command = "REGISTER|testuser|testpass\n";

        ASSERT_EQ(send(sock, register_command.c_str(), register_command.size(), 0),
                  static_cast<ssize_t>(register_command.size()));

        const std::string response = receive_response();

        ASSERT_EQ(response.rfind("REGISTER_OK|", 0), 0) << "Registration failed: " << response;
    }

    void TearDown() override {
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }

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

    std::string receive_response() {
        char buffer[4096];

        const ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            return "";
        }

        buffer[bytes] = '\0';

        return std::string(buffer);
    }

    std::string send_command(const std::string& command) {
        const ssize_t sent = send(sock, command.c_str(), command.size(), 0);

        if (sent != static_cast<ssize_t>(command.size())) {
            return "";
        }

        return receive_response();
    }

    int connect_and_register(const std::string& user, const std::string& password) {
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

        const std::string command = "REGISTER|" + user + "|" + password + "\n";

        if (send(socket_fd, command.c_str(), command.size(), 0) !=
            static_cast<ssize_t>(command.size())) {
            close(socket_fd);
            return -1;
        }

        char buffer[1024];

        const ssize_t bytes = recv(socket_fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0) {
            close(socket_fd);
            return -1;
        }

        buffer[bytes] = '\0';

        const std::string response(buffer);

        if (response.rfind("REGISTER_OK|", 0) != 0) {
            close(socket_fd);
            return -1;
        }

        return socket_fd;
    }
};

TEST_F(ServerClientTest, CompleteKeyValueLifecycle) {
    EXPECT_EQ(send_command("SET|name|Alice\n"), "OK\n");

    EXPECT_EQ(send_command("GET|name\n"), "Alice\n");

    EXPECT_EQ(send_command("DEL|name\n"), "OK\n");

    EXPECT_EQ(send_command("GET|name\n"), "NOT_FOUND\n");
}

TEST_F(ServerClientTest, InvalidCommands) {
    EXPECT_EQ(send_command("SET||value\n"), "REJECT|INVALID_INPUT|EMPTY_KEY\n");

    EXPECT_EQ(send_command("SET|key|\n"), "REJECT|INVALID_INPUT|EMPTY_VALUE\n");

    EXPECT_EQ(send_command("GET|\n"), "REJECT|INVALID_INPUT|EMPTY_KEY\n");

    EXPECT_EQ(send_command("DEL|\n"), "REJECT|INVALID_INPUT|EMPTY_KEY\n");

    EXPECT_EQ(send_command("INVALID_COMMAND\n"), "ERROR|INVALID_COMMAND\n");

    // A valid command must still work after invalid requests.
    EXPECT_EQ(send_command("SET|recovery|works\n"), "OK\n");

    EXPECT_EQ(send_command("GET|recovery\n"), "works\n");
}

TEST_F(ServerClientTest, MultipleRequests) {
    const std::string batch =
        "SET|key1|value1\n"
        "SET|key2|value2\n"
        "GET|key1\n"
        "GET|key2\n"
        "DEL|key1\n"
        "GET|key1\n";

    ASSERT_EQ(send(sock, batch.c_str(), batch.size(), 0), static_cast<ssize_t>(batch.size()));

    std::string all_responses;
    char buffer[4096];

    timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    // Timeout prevents the test from hanging if the server stops responding.
    ASSERT_EQ(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

    // Six commands should produce six newline-terminated responses.
    while (std::count(all_responses.begin(), all_responses.end(), '\n') < 6) {
        const ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

        ASSERT_GT(bytes, 0) << "Connection stalled before all responses arrived";

        buffer[bytes] = '\0';
        all_responses.append(buffer, static_cast<size_t>(bytes));
    }

    EXPECT_NE(all_responses.find("OK\n"), std::string::npos);

    EXPECT_NE(all_responses.find("value1\n"), std::string::npos);

    EXPECT_NE(all_responses.find("value2\n"), std::string::npos);

    EXPECT_NE(all_responses.find("NOT_FOUND\n"), std::string::npos);
}

TEST_F(ServerClientTest, UserIsolation) {
    const int bob_sock = connect_and_register("bob", "bobpass");

    ASSERT_GE(bob_sock, 0);

    // Alice stores a value.
    EXPECT_EQ(send_command("SET|shared|alice-value\n"), "OK\n");

    // Bob has a different connection and user identity.
    const std::string bob_set = "SET|shared|bob-value\n";

    ASSERT_EQ(send(bob_sock, bob_set.c_str(), bob_set.size(), 0),
              static_cast<ssize_t>(bob_set.size()));

    char buffer[1024];

    ssize_t bytes = recv(bob_sock, buffer, sizeof(buffer) - 1, 0);

    ASSERT_GT(bytes, 0);

    buffer[bytes] = '\0';

    EXPECT_EQ(std::string(buffer), "OK\n");

    // Alice sees her own value.
    EXPECT_EQ(send_command("GET|shared\n"), "alice-value\n");

    // Bob sees his own value.
    const std::string bob_get = "GET|shared\n";

    ASSERT_EQ(send(bob_sock, bob_get.c_str(), bob_get.size(), 0),
              static_cast<ssize_t>(bob_get.size()));

    bytes = recv(bob_sock, buffer, sizeof(buffer) - 1, 0);

    ASSERT_GT(bytes, 0);

    buffer[bytes] = '\0';

    EXPECT_EQ(std::string(buffer), "bob-value\n");

    close(bob_sock);
}

TEST_F(ServerClientTest, LoginWithRegisteredUser) {
    // First connection already registered the test user.
    close(sock);
    sock = -1;

    sock = connect_to_server();

    ASSERT_GE(sock, 0);

    // SetUp() registers the first and only user on this server instance,
    // so ID 1 is guaranteed.
    const std::string login_command = "LOGIN|1|testpass\n";

    ASSERT_EQ(send(sock, login_command.c_str(), login_command.size(), 0),
              static_cast<ssize_t>(login_command.size()));

    const std::string response = receive_response();

    // LOGIN response is two lines: "AUTH_OK|<id>\n" followed by "Hi <name>\n".
    EXPECT_TRUE(response.rfind("AUTH_OK|1\nHi testuser\n", 0) == 0)
        << "Unexpected login response: " << response;

    EXPECT_EQ(send_command("SET|login_test|success\n"), "OK\n");

    EXPECT_EQ(send_command("GET|login_test\n"), "success\n");
}

TEST_F(ServerClientTest, InvalidLoginRejected) {
    // Close the authenticated registration connection.
    close(sock);
    sock = -1;

    sock = connect_to_server();

    ASSERT_GE(sock, 0);

    const std::string login_command = "LOGIN|1|wrong_password\n";

    ASSERT_EQ(send(sock, login_command.c_str(), login_command.size(), 0),
              static_cast<ssize_t>(login_command.size()));

    const std::string response = receive_response();

    EXPECT_EQ(response, "AUTH_REJECT|INVALID_CREDENTIALS\n");
}
