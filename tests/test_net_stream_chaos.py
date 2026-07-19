# tests/test_stream_chaos.py
# -----------------------------------------------------------------------------
# GOAL: Stress Test the TCP Protocol Parser (Resilience).
# WHY PYTHON?: 
#   Python allows raw socket manipulation (sending partial bytes, sleeping mid-packet)
#   much easier than C++. We act as a "Bad Client" to break the server.
#
# SCENARIOS TESTED:
#   1. Fragmentation: Sending 1 byte at a time (Simulating slow networks/latency).
#   2. Sticky Packets: Sending 50 requests in one TCP burst (Simulating high throughput).
#   3. Partial Reads: Cutting a command in half to ensure the server buffers correctly.
# -----------------------------------------------------------------------------

import socket
import time
import random
import sys

HOST = '127.0.0.1'
PORT = 8080

def get_socket():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        return s
    except ConnectionRefusedError:
        print(f"[ERROR] Could not connect to {HOST}:{PORT}. Is the server running?")
        sys.exit(1)

def test_fragmentation():
    """Test 1: Send a command 1 byte at a time"""
    print(f"[TEST 1] Fragmentation (Drip Feed)...", end=" ", flush=True)

    s = get_socket()
    cmd = "SET chaos_frag_key 100"

    try:
        # Send characters one by one with a delay
        # The Server must buffer these until it sees the newline '\n'
        for char in cmd:
            s.send(char.encode())
            time.sleep(0.01)    # 10ms delay triggers OS to send small packets

        s.send(b'\n')   # Finally send the delimiter
        response = s.recv(1024).decode()
        
        if "OK" in response:
            print("OK")
        else:
            print(f"FAILED\n   -> Expected 'OK', got '{response.strip()}'")

    except Exception as e:
        print(f"ERROR\n   -> {e}")
    finally:
        s.close()

def test_sticky_packets():
    """Test 2: Send 50 commands in ONE massive TCP packet"""
    print(f"[TEST 2] Sticky Packets (Batch Write)...", end=" ", flush=True)
    s = get_socket()

    count = 50
    payload = ""
    # Construct a giant payload: "SET k1 v1\nSET k2 v2\n..."
    for i in range(count):
        payload += f"SET batch_{i} {i}\n"

    try:
        # sendall() pushes everything to the kernel buffer at once.
        # The server likely receives this as one or two large reads, not 50 small ones.
        s.sendall(payload.encode())

        received_data = ""
        start_time = time.time()
        
        # Wait until we get all responses or timeout
        while received_data.count("OK") < count:
            chunk = s.recv(4096).decode()
            if not chunk: break
            received_data += chunk
            if time.time() - start_time > 2:    # 2s Timeout
                break

        oks = received_data.count("OK")
        if oks == count:
            print(f"OK ({count}/{count} cmds)")
        else:
            print(f"FAILED\n   -> Sent {count}, received {oks} OKs")
            
    except Exception as e:
        print(f"ERROR\n   -> {e}")
    finally:
        s.close()

def test_random_split():
    """Test 3: Randomly split a command in two parts"""
    print(f"[TEST 3] Random Split Boundary...", end=" ", flush=True)
    s = get_socket()
    
    cmd = "GET chaos_frag_key\n"
    # Cut the string at a random position (e.g., "GET cha" | "os_frag_key")
    cut_pos = random.randint(1, len(cmd) - 2)
    
    part1 = cmd[:cut_pos]
    part2 = cmd[cut_pos:]
    
    try:
        s.send(part1.encode())
        time.sleep(0.05)    # Force the server to read partial data
        s.send(part2.encode())
        
        response = s.recv(1024).decode()
        if "100" in response: 
            print("OK")
        else:
            print(f"FAILED\n   -> Split at index {cut_pos}, got '{response.strip()}'")
            
    except Exception as e:
        print(f"ERROR\n   -> {e}")
    finally:
        s.close()

if __name__ == "__main__":
    print("\n\n" + "="*60)
    print("         TEST: TCP STREAM RESILIENCE (CHAOS)                ")
    print("="*60)
    
    # Setup data
    s = get_socket()
    s.send(b"SET chaos_frag_key 100\n")
    s.recv(1024)
    s.close()

    test_fragmentation()
    test_sticky_packets()
    test_random_split()
    
    print("=" * 60 + "\n\n")