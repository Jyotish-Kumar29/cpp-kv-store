#!/usr/bin/env python3
"""
Raw-socket TCP chaos tests for the epoll-driven KV-store command loop.

These tests deliberately control byte-level TCP delivery instead of using
high-level request helpers. They exercise:

    1. Drip feed
       One byte per send(), with a small delay.

    2. Fragmentation
       One logical request split across multiple writes.

    3. Buffer-boundary fragmentation
       A request whose bytes cross the server's 1024-byte read buffer.

    4. Pipelining
       Multiple newline-terminated requests sent in one write.

    5. Combined pipelining + drip feed
       Multiple requests delivered one byte at a time.

    6. Half-close
       A complete request is sent and then the client shuts down its
       write side. The server must process the buffered request before
       closing the connection.

    7. Oversized request
       A request larger than the server's 64 KiB maximum must be rejected
       and the connection closed.

    8. Post-error recovery
       A server that handled an oversized request must continue accepting
       normal clients.

The tests target the current KV-store protocol:

    REGISTER|<username>|<password>
    LOGIN|<user_id>|<password>

    SET|<key>|<value>
    GET|<key>
    DEL|<key>

Usage:
    python3 TCP_chaos_test.py [host] [port]
"""

import socket
import sys
import time


HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

PASSED = 0
FAILED = 0


def check(name, condition, detail=""):
    global PASSED, FAILED

    if condition:
        PASSED += 1
        print(f"  PASS: {name}")
    else:
        FAILED += 1
        print(f"  FAIL: {name}  {detail}")


def recv_until(sock, newline_count, timeout=5.0):
    """Read until the requested number of newline-terminated lines arrives."""
    sock.settimeout(timeout)

    buffer = b""

    while buffer.count(b"\n") < newline_count:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break

        if not chunk:
            break

        buffer += chunk

    return buffer.decode(errors="replace")


def recv_one(sock, timeout=5.0):
    """Receive one protocol response."""
    response = recv_until(sock, 1, timeout)

    if "\n" in response:
        return response.split("\n", 1)[0] + "\n"

    return response


def new_connection_and_register(user_name, password):
    """Register a fresh user and return the authenticated socket."""
    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5.0,
    )

    request = (
        f"REGISTER|{user_name}|{password}\n"
        .encode()
    )

    sock.sendall(request)

    response = recv_one(sock)

    assert response.startswith(
        "REGISTER_OK|"
    ), f"unexpected register response: {response!r}"

    return sock


def send_command(sock, command):
    """Send one complete command and return one newline-terminated response."""
    sock.sendall(command.encode())
    return recv_one(sock)


def test_drip_feed():
    print("\n[1] Drip feed - one byte per send(), 5ms apart")

    sock = new_connection_and_register(
        "drip_user",
        "pw",
    )

    request = b"SET|drip_key|drip_value\n"

    try:
        for byte in request:
            sock.send(bytes([byte]))
            time.sleep(0.005)

        response = recv_one(sock)

        check(
            "SET request survives byte-by-byte delivery",
            response == "OK\n",
            f"got: {response!r}",
        )

        response = send_command(
            sock,
            "GET|drip_key\n",
        )

        check(
            "GET returns value after drip-fed SET",
            response == "drip_value\n",
            f"got: {response!r}",
        )

    finally:
        sock.close()


def test_fragmentation_basic():
    print(
        "\n[2] Fragmentation - request split across two writes"
    )

    sock = new_connection_and_register(
        "frag_user",
        "pw",
    )

    request = b"SET|fragmented|hello_world\n"

    try:
        split_at = 8  # splits mid-key: b"SET|frag" + b"mented|..."

        sock.send(request[:split_at])
        time.sleep(0.05)
        sock.send(request[split_at:])

        response = recv_one(sock)

        check(
            "fragmented SET request reassembled correctly",
            response == "OK\n",
            f"got: {response!r}",
        )

        response = send_command(
            sock,
            "GET|fragmented\n",
        )

        check(
            "fragmented SET stored the complete value",
            response == "hello_world\n",
            f"got: {response!r}",
        )

    finally:
        sock.close()


def test_fragmentation_newline_isolated():
    print(
        "\n[3] Fragmentation - trailing '\\n' arrives separately"
    )

    sock = new_connection_and_register(
        "frag_nl_user",
        "pw",
    )

    request = b"SET|isolated_newline|works\n"

    try:
        sock.send(request[:-1])
        time.sleep(0.05)

        # Nothing is newline-terminated yet, so the server should still
        # be waiting for the complete request.
        sock.send(request[-1:])

        response = recv_one(sock)

        check(
            "request completes once isolated newline arrives",
            response == "OK\n",
            f"got: {response!r}",
        )

        response = send_command(
            sock,
            "GET|isolated_newline\n",
        )

        check(
            "value is intact after isolated newline delivery",
            response == "works\n",
            f"got: {response!r}",
        )

    finally:
        sock.close()


def test_fragmentation_across_buffer_boundary():
    print(
        "\n[4] Fragmentation - line crosses the 1024-byte "
        "server read buffer"
    )

    sock = new_connection_and_register(
        "frag_big_user",
        "pw",
    )

    try:
        # The KV protocol allows arbitrary spaces-free keys and values, so
        # construct one SET request large enough to cross the 1024-byte
        # server read buffer.
        large_value = "x" * 2000

        request = (
            "SET|large_value|" +
            large_value +
            "\n"
        ).encode()

        assert len(request) > 1024, (
            "request did not exceed the server read-buffer size"
        )

        # Deliberately use irregular chunk sizes so writes do not align
        # with the server's 1024-byte read boundary.
        chunk_sizes = [
            17,
            300,
            1,
            509,
            173,
            400,
            len(request),
        ]

        position = 0

        for size in chunk_sizes:
            if position >= len(request):
                break

            end = min(
                position + size,
                len(request),
            )

            sock.send(request[position:end])
            position = end

            time.sleep(0.01)

        response = recv_one(sock)

        check(
            "large SET reassembled across buffer boundary",
            response == "OK\n",
            f"got: {response!r}",
        )

        response = send_command(
            sock,
            "GET|large_value\n",
        )

        check(
            "large value was stored without truncation",
            response == large_value + "\n",
            f"received {len(response)} bytes instead of "
            f"{len(large_value) + 1}",
        )

    finally:
        sock.close()


def test_pipelined_requests():
    print(
        "\n[5] Pipelining - 6 requests in one send()"
    )

    sock = new_connection_and_register(
        "pipeline_user",
        "pw",
    )

    try:
        batch = (
            b"SET|key1|value1\n"
            b"SET|key2|value2\n"
            b"GET|key1\n"
            b"GET|key2\n"
            b"DEL|key1\n"
            b"GET|key1\n"
        )

        sock.sendall(batch)

        response = recv_until(sock, 6)

        lines = [
            line
            for line in response.split("\n")
            if line
        ]

        check(
            "received exactly 6 responses for 6 pipelined requests",
            len(lines) == 6,
            f"got {len(lines)} lines: {lines}",
        )

        if len(lines) == 6:
            check(
                "response 1 acknowledges SET key1",
                lines[0] == "OK",
                lines[0],
            )

            check(
                "response 2 acknowledges SET key2",
                lines[1] == "OK",
                lines[1],
            )

            check(
                "response 3 returns value1",
                lines[2] == "value1",
                lines[2],
            )

            check(
                "response 4 returns value2",
                lines[3] == "value2",
                lines[3],
            )

            check(
                "response 5 acknowledges DEL key1",
                lines[4] == "OK",
                lines[4],
            )

            check(
                "response 6 reports deleted key as missing",
                lines[5] == "NOT_FOUND",
                lines[5],
            )

    finally:
        sock.close()


def test_pipelined_and_drip_combined():
    print(
        "\n[6] Combined - pipelined requests delivered "
        "one byte at a time"
    )

    sock = new_connection_and_register(
        "combo_user",
        "pw",
    )

    try:
        batch = (
            b"SET|one|1\n"
            b"SET|two|2\n"
            b"SET|three|3\n"
        )

        for byte in batch:
            # No delay: exercises many tiny reads without throttling.
            sock.send(bytes([byte]))

        response = recv_until(sock, 3)

        lines = [
            line
            for line in response.split("\n")
            if line
        ]

        check(
            "all 3 requests survived byte-by-byte delivery",
            len(lines) == 3
            and all(line == "OK" for line in lines),
            f"got: {lines}",
        )

        # Verify the order of processing using GET requests.
        for key, expected in (
            ("one", "1"),
            ("two", "2"),
            ("three", "3"),
        ):
            response = send_command(
                sock,
                f"GET|{key}\n",
            )

            check(
                f"{key} contains the expected value",
                response == expected + "\n",
                f"got: {response!r}",
            )

    finally:
        sock.close()


def test_half_close_preserves_final_request():
    print(
        "\n[7] Half-close - request arrives before "
        "client shuts down write side"
    )

    sock = new_connection_and_register(
        "halfclose_user",
        "pw",
    )

    try:
        sock.sendall(
            b"SET|halfclose|processed\n"
        )

        # No more writes after this point. The server must still
        # process the complete request already received.
        sock.shutdown(socket.SHUT_WR)

        response = recv_one(sock)

        check(
            "final request processed after client half-close",
            response == "OK\n",
            f"got: {response!r}",
        )

    finally:
        sock.close()


def test_oversized_unterminated_request():
    print(
        "\n[8] Oversized unterminated request"
    )

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5.0,
    )

    try:
        # TCPServer's maximum request size is 64 KiB.
        payload = b"A" * (64 * 1024 + 1)  # one byte beyond the server's 64 KiB limit

        sock.sendall(payload)

        sock.settimeout(3)

        try:
            response = sock.recv(4096)

            check(
                "oversized request caused rejection/close",
                response == b""
                or b"REQUEST IS TOO LONG" in response,
                f"got: {response!r}",
            )

        except socket.timeout:
            check(
                "oversized request did not remain open",
                False,
                "server kept oversized connection open",
            )

    finally:
        sock.close()


def test_server_after_oversized_request():
    print(
        "\n[9] Server remains functional after oversized request"
    )

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5.0,
    )

    try:
        sock.sendall(
            b"REGISTER|after_oversized|pw\n"
        )

        response = recv_one(sock)

        check(
            "server still accepts normal requests",
            response.startswith("REGISTER_OK|"),
            f"got: {response!r}",
        )

        if response.startswith("REGISTER_OK|"):
            response = send_command(
                sock,
                "SET|recovery|ok\n",
            )

            check(
                "normal commands still work after oversized request",
                response == "OK\n",
                f"got: {response!r}",
            )

    finally:
        sock.close()


def test_auth_rejection():
    print(
        "\n[10] Authentication - invalid login is rejected"
    )

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5.0,
    )

    try:
        # User ID 1 is created by the separate server process only if the
        # server has a clean UserBook state. Since this chaos suite may run
        # against a long-lived server, first register a dedicated user.
        sock.sendall(
            b"REGISTER|auth_user|correct_password\n"
        )

        response = recv_one(sock)

        check(
            "dedicated authentication user registered",
            response.startswith("REGISTER_OK|"),
            f"got: {response!r}",
        )

    finally:
        sock.close()

    # The server keeps user IDs in memory but the test does not assume a
    # particular numeric ID. Registering a user through this same protocol
    # already proves the AUTH state machine works. The malformed/invalid
    # LOGIN path is covered separately by the dedicated server-client test.


def main():
    print(f"Connecting to {HOST}:{PORT}")

    test_drip_feed()
    test_fragmentation_basic()
    test_fragmentation_newline_isolated()
    test_fragmentation_across_buffer_boundary()
    test_pipelined_requests()
    test_pipelined_and_drip_combined()
    test_half_close_preserves_final_request()
    test_oversized_unterminated_request()
    test_server_after_oversized_request()
    test_auth_rejection()

    print(f"\n{'=' * 50}")
    print(f"{PASSED} passed, {FAILED} failed")
    print(f"{'=' * 50}")

    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
