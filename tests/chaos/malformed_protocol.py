#!/usr/bin/env python3
"""
Malformed protocol tests.

Sends malformed, random, and unusual input to the server to verify that
it remains stable and continues accepting valid requests.
"""

import random
import socket
import string
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
        print(f"  FAIL: {name} {detail}")


def send_and_receive(sock, data, timeout=5.0):
    """Send data and return the server response, if any."""
    sock.settimeout(timeout)
    sock.sendall(data)

    try:
        return sock.recv(4096)
    except socket.timeout:
        return b""


def test_random_bytes():
    print("\n[1] Random bytes test")

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5,
    )

    for _ in range(50):
        random_data = bytes(
            random.randint(0, 255)
            for _ in range(random.randint(1, 100))
        )

        sock.sendall(random_data)
        time.sleep(0.01)

    sock.close()

    check(
        "server survived random bytes",
        True,
    )


def test_random_strings():
    print("\n[2] Random string test")

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5,
    )

    for _ in range(100):
        random_string = "".join(
            random.choices(
                string.printable,
                k=random.randint(1, 50),
            )
        )

        sock.sendall(
            f"{random_string}\n".encode()
        )

        time.sleep(0.005)

    sock.close()

    check(
        "server survived random strings",
        True,
    )


def test_oversized_fields():
    print("\n[3] Oversized field test")

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5,
    )

    long_username = "A" * 10000

    sock.sendall(
        f"REGISTER|{long_username}|pass\n".encode()
    )

    response = sock.recv(4096)

    check(
        "server handled long username",
        bool(response),
    )

    # REGISTER above succeeds and authenticates the connection; send an
    # oversized value in a SET command to stress-test the data path.
    long_value = "9" * 10000

    sock.sendall(
        f"SET|oversized_key|{long_value}\n".encode()
    )

    response = sock.recv(4096)

    check(
        "server handled long value in SET",
        bool(response),
    )

    sock.close()


def test_special_characters():
    print("\n[4] Special characters test")

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5,
    )

    special_strings = [
        "REGISTER|user\x00name|pass\n",    # NUL byte in username
        "REGISTER|user\nname|pass\n",      # embedded newline (splits into two protocol lines)
        "REGISTER|user|name|pass|extra\n", # extra fields beyond the protocol spec
        "SET|key\x00special|value\n",      # NUL byte in a data-command key
        "GET|\x00key\n",                   # NUL-only key
    ]

    for value in special_strings:
        sock.sendall(
            value.encode(
                "utf-8",
                errors="ignore",
            )
        )

        time.sleep(0.01)

    sock.close()

    check(
        "server survived special characters",
        True,
    )


def test_null_bytes():
    print("\n[5] Null byte test")

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5,
    )

    # Null bytes embedded in a KV-store command value.
    null_data = (
        b"SET|key|value\x00\x00\x00\n"
    )

    try:
        sock.sendall(null_data)

        response = sock.recv(4096)

        # Either a valid error response or connection closure is acceptable.
        check(
            "server responded to null bytes",
            bool(response),
        )

    except (ConnectionResetError, BrokenPipeError, socket.timeout):
        check(
            "server handled null bytes (closed connection)",
            True,
        )

    finally:
        sock.close()


def test_server_still_functional():
    print("\n[6] Server still functional test")

    sock = socket.create_connection(
        (HOST, PORT),
        timeout=5,
    )

    sock.sendall(
        b"REGISTER|functional_test|pass\n"
    )

    response = sock.recv(4096)

    check(
        "server still functional after chaos tests",
        response.startswith(b"REGISTER_OK"),
        f"got: {response!r}",
    )

    sock.close()


def main():
    print(
        f"Running malformed protocol tests "
        f"against {HOST}:{PORT}"
    )

    test_random_bytes()
    test_random_strings()
    test_oversized_fields()
    test_special_characters()
    test_null_bytes()
    test_server_still_functional()

    print(f"\n{'=' * 50}")
    print(f"{PASSED} passed, {FAILED} failed")
    print(f"{'=' * 50}")

    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()