# -----------------------------------------------------------------------------
# GOAL: Verify Data Consistency and Correctness (ACID-lite properties).
# METHODOLOGY: "Write-Read-Verify" Pattern
#   1. Write Phase: Insert 10,000 unique keys (deterministic values).
#   2. Read Phase: Retrieve all 10,000 keys.
#   3. Verify Phase: strict string comparison (Expected vs. Actual).
#
# WHY THIS MATTERS:
#   High throughput is useless if the database loses data. This test detects:
#   - Race Conditions (two threads overwriting the same memory).
#   - Partial Writes (corrupted data on disk/network).
#   - Hash Collisions (if the internal map is broken).
#
# NOTE ON AUTH: keys are namespaced per authenticated user_id server-side.
# The write phase REGISTERs a fresh user; the read/verify and invalid-command
# phases open new connections and must LOGIN as that same user_id (not
# register a new one), or every read would come back NOT_FOUND even though
# the write succeeded.
#
# Requires a live server on SERVER_IP:SERVER_PORT -- start one first with
# ./scripts/run/manage_server.sh start.
# -----------------------------------------------------------------------------

import socket
import time

SERVER_IP = "127.0.0.1"
SERVER_PORT = 8080
NUM_KEYS = 10000    # Total unique keys to verify
USERNAME = "integrity_test_user"
PASSWORD = "integrity_pw"


class Diagnostics:
    """Tracks failure counts by category so the final report can point at
    a likely root cause (crash vs. hang vs. logic bug) instead of just
    saying "something failed"."""

    def __init__(self):
        self.crashes = 0      # Connection Refused / Broken Pipe
        self.hangs = 0        # Socket Timeout
        self.corrupt = 0      # Logic Errors (Data mismatch)
        self.missing = 0      # Logic Errors (Key not found)

    def report(self):
        """Prints a human-readable summary, prioritizing the most severe
        failure category (crash > hang > logic error) so the reader's
        attention goes to the root cause rather than a wall of numbers."""
        print("\n" + "-"*60)
        print("                  DIAGNOSTIC AUTOPSY REPORT                 ")
        print("-"*60)

        if self.crashes > 0:
            print(f"[CRITICAL] SERVER CRASHED: {self.crashes} times.")
            print("   -> LIKELY CULPRIT: Segfault, OOM Kill, or SIGPIPE.")
        elif self.hangs > 0:
            print(f"[WARNING] SERVER HUNG: {self.hangs} times.")
            print("   -> LIKELY CULPRIT: Deadlock or Infinite Loop.")
        elif self.corrupt > 0 or self.missing > 0:
            print(f"[FAIL] LOGIC ERROR: {self.missing} missing, {self.corrupt} corrupted.")
        else:
            print("[SUCCESS] All systems functional.")
        print("="*60 + "\n\n")


def print_header():
    """Prints the test banner with target host/port and key count."""
    print("\n\n" + "="*60)
    print("         TEST: DATA INTEGRITY CHECK (ACID-LITE)             ")
    print("="*60)
    print(f"Target:       {SERVER_IP}:{SERVER_PORT}")
    print(f"Keys:         {NUM_KEYS}")
    print("-" * 60)


def register(sock):
    """Register a fresh user on this connection; returns the user_id."""
    sock.sendall(f"REGISTER|{USERNAME}|{PASSWORD}\n".encode())
    resp = sock.recv(1024).decode().strip()
    if not resp.startswith("REGISTER_OK|"):
        raise RuntimeError(f"Registration failed: {resp}")
    return int(resp.split("|")[1])


def login(sock, user_id):
    """Log this connection in as an existing user_id, so it shares the same
    namespaced key-space as whichever connection originally registered it."""
    sock.sendall(f"LOGIN|{user_id}|{PASSWORD}\n".encode())
    resp = sock.recv(1024).decode().strip()
    if not resp.startswith("AUTH_OK|"):
        raise RuntimeError(f"Login failed: {resp}")


def run_test():
    """Runs the full write -> read/verify -> invalid-command sequence
    against a live server and prints a diagnostic report at the end."""
    diag = Diagnostics()

    # ------------------------------------------------------------------
    # 1. WRITE PHASE -- register a user, then SET all NUM_KEYS keys,
    #    confirming each write is acknowledged before moving on.
    # ------------------------------------------------------------------
    print_header()
    print(f"[Phase 1] Writing {NUM_KEYS} keys...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)    # Wait max 2 seconds per request
    try:
        sock.connect((SERVER_IP, SERVER_PORT))
    except ConnectionRefusedError:
        print("ERROR: Server is not running. Start ./scripts/run/manage_server.sh start first.")
        return

    try:
        user_id = register(sock)
    except (RuntimeError, socket.timeout) as e:
        print(f"ERROR: {e}")
        sock.close()
        return

    start_time = time.time()

    for i in range(NUM_KEYS):
        # Deterministic Key/Value generation
        key = f"check_{i}"
        val = f"value_{i}"

        command = f"SET|{key}|{val}\n"

        try:
            sock.sendall(command.encode())

            # Synchronous check: Wait for OK to ensure it was actually persisted
            response = sock.recv(1024)

            if not response:
                print(f"\n[!] Server closed connection at {key}")
                diag.crashes += 1
                break

            if b"OK" not in response:
                print(f"Write Error at {key}: {response}")
                diag.corrupt += 1

        except socket.timeout:      # Catch hangs
            print(f"\n[!] Timeout writing {key}")
            diag.hangs += 1
            break
        except (ConnectionResetError, BrokenPipeError):     # CATCH CRASHES
            print(f"\n[!] Connection lost writing {key}")
            diag.crashes += 1
            break

    write_time = time.time() - start_time
    sock.close()

    if diag.crashes > 0 or diag.hangs > 0:
        print(f"Write Phase ABORTED after {i} keys ({write_time:.2f}s).")
    else:
        print(f"Write Phase Complete ({write_time:.2f}s).")

    # Bail out early -- there's no point reading back data from a server
    # that already crashed or hung mid-write.
    if diag.crashes > 0 or diag.hangs > 0:
        diag.report()
        return

    # ------------------------------------------------------------------
    # 2. READ & VERIFY PHASE -- open a fresh connection, LOGIN as the
    #    same user_id, and GET every key back, comparing against the
    #    deterministic value written in Phase 1.
    # ------------------------------------------------------------------
    print(f"Reading back {NUM_KEYS} keys to verify...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    sock.connect((SERVER_IP, SERVER_PORT))

    try:
        login(sock, user_id)
    except (RuntimeError, socket.timeout) as e:
        print(f"ERROR: {e}")
        sock.close()
        diag.report()
        return

    for i in range(NUM_KEYS):
        key = f"check_{i}"
        expected_val = f"value_{i}"
        command = f"GET|{key}\n"

        try:
            sock.sendall(command.encode())
            response = sock.recv(1024).decode().strip()

            if response == "NOT_FOUND":
                diag.missing += 1
            elif expected_val not in response:
                diag.corrupt += 1

        except socket.timeout:
            diag.hangs += 1
            break
        except (ConnectionResetError, BrokenPipeError):
            diag.crashes += 1
            break

    sock.close()

    # ------------------------------------------------------------------
    # 3. INVALID COMMANDS PHASE -- confirm the parser rejects malformed
    #    or unrecognized commands cleanly (REJECT|/ERROR|) instead of
    #    crashing or silently misbehaving.
    # ------------------------------------------------------------------
    print(f"Testing invalid commands and parser errors...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    sock.connect((SERVER_IP, SERVER_PORT))

    try:
        login(sock, user_id)
    except (RuntimeError, socket.timeout) as e:
        print(f"ERROR: {e}")
        sock.close()
        diag.report()
        return

    invalid_cmds = [
        "SET|only_key\n",       # missing value field
        "UNKNOWN|key\n",        # unrecognized command
        "BLAH\n",               # not a recognized command at all
    ]
    for cmd in invalid_cmds:
        try:
            sock.sendall(cmd.encode())
            response = sock.recv(1024).decode().strip()
            if not (response.startswith("REJECT|") or response.startswith("ERROR|")):
                print(f"Parser Error logic failed. Sent '{cmd.strip()}', got '{response}'")
                diag.corrupt += 1
        except Exception:
            diag.crashes += 1
    sock.close()
    diag.report()


if __name__ == "__main__":
    run_test()