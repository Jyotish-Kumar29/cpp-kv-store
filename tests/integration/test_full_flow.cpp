#include <gtest/gtest.h>

#include <TCPServer.hpp>
#include <cstdint>
#include <string>

// In-process integration tests: calls process_request() directly on a
// TCPServer instance, bypassing the TCP stack entirely. Tests command
// parsing, user isolation, and error handling logic end-to-end.
class FullFlowTest : public ::testing::Test {
protected:
    TCPServer server;

    // port=0: not listening (in-process use only); persistent=false: in-memory only.
    FullFlowTest() : server(0, false) {}
};

TEST_F(FullFlowTest, CompleteUserAndKeyValueLifecycle) {
    const uint64_t alice = server.register_user("alice", "pass1");
    const uint64_t bob = server.register_user("bob", "pass2");

    ASSERT_EQ(alice, 1);  // IDs are monotonically assigned from 1 (0 is the unauthenticated sentinel)
    ASSERT_EQ(bob, 2);

    std::string response;

    // Alice stores and retrieves a value.
    server.process_request("SET|name|Alice", response, alice);
    EXPECT_EQ(response, "OK\n");

    server.process_request("GET|name", response, alice);
    EXPECT_EQ(response, "Alice\n");

    // User data is isolated.
    server.process_request("GET|name", response, bob);
    EXPECT_EQ(response, "NOT_FOUND\n");

    // Bob can use the same logical key independently.
    server.process_request("SET|name|Bob", response, bob);
    EXPECT_EQ(response, "OK\n");

    server.process_request("GET|name", response, bob);
    EXPECT_EQ(response, "Bob\n");

    // Alice's value remains unchanged.
    server.process_request("GET|name", response, alice);
    EXPECT_EQ(response, "Alice\n");

    // Delete Alice's key.
    server.process_request("DEL|name", response, alice);
    EXPECT_EQ(response, "OK\n");

    server.process_request("GET|name", response, alice);
    EXPECT_EQ(response, "NOT_FOUND\n");

    // Bob's data remains intact.
    server.process_request("GET|name", response, bob);
    EXPECT_EQ(response, "Bob\n");
}

TEST_F(FullFlowTest, MultipleKeysAndUsers) {
    const uint64_t alice = server.register_user("alice", "pass1");
    const uint64_t bob = server.register_user("bob", "pass2");
    const uint64_t charlie = server.register_user("charlie", "pass3");

    std::string response;

    server.process_request("SET|city|Delhi", response, alice);
    EXPECT_EQ(response, "OK\n");

    server.process_request("SET|language|C++", response, alice);
    EXPECT_EQ(response, "OK\n");

    server.process_request("SET|project|kv-store", response, alice);
    EXPECT_EQ(response, "OK\n");

    server.process_request("SET|city|Mumbai", response, bob);
    EXPECT_EQ(response, "OK\n");

    server.process_request("SET|city|Bengaluru", response, charlie);
    EXPECT_EQ(response, "OK\n");

    EXPECT_NO_FATAL_FAILURE({
        server.process_request("GET|city", response, alice);
        EXPECT_EQ(response, "Delhi\n");

        server.process_request("GET|language", response, alice);
        EXPECT_EQ(response, "C++\n");

        server.process_request("GET|project", response, alice);
        EXPECT_EQ(response, "kv-store\n");

        server.process_request("GET|city", response, bob);
        EXPECT_EQ(response, "Mumbai\n");

        server.process_request("GET|city", response, charlie);
        EXPECT_EQ(response, "Bengaluru\n");

        server.process_request("GET|language", response, bob);
        EXPECT_EQ(response, "NOT_FOUND\n");

        server.process_request("GET|project", response, charlie);
        EXPECT_EQ(response, "NOT_FOUND\n");
    });
}

TEST_F(FullFlowTest, ErrorRecovery) {
    const uint64_t user = server.register_user("test", "pass");

    std::string response;

    // Invalid SET requests.
    server.process_request("SET||value", response, user);
    EXPECT_EQ(response, "REJECT|INVALID_INPUT|EMPTY_KEY\n");

    server.process_request("SET|key|", response, user);
    EXPECT_EQ(response, "REJECT|INVALID_INPUT|EMPTY_VALUE\n");

    // Invalid GET/DEL requests.
    server.process_request("GET|", response, user);
    EXPECT_EQ(response, "REJECT|INVALID_INPUT|EMPTY_KEY\n");

    server.process_request("DEL|", response, user);
    EXPECT_EQ(response, "REJECT|INVALID_INPUT|EMPTY_KEY\n");

    // Unknown command.
    server.process_request("UNKNOWN|key|value", response, user);
    EXPECT_EQ(response, "ERROR|INVALID_COMMAND\n");

    // Valid operations must still work afterwards.
    server.process_request("SET|key|value", response, user);
    EXPECT_EQ(response, "OK\n");

    server.process_request("GET|key", response, user);
    EXPECT_EQ(response, "value\n");

    server.process_request("DEL|key", response, user);
    EXPECT_EQ(response, "OK\n");

    server.process_request("GET|key", response, user);
    EXPECT_EQ(response, "NOT_FOUND\n");

    // Deleting an absent key.
    server.process_request("DEL|key", response, user);
    EXPECT_EQ(response, "NOT_FOUND\n");
}
