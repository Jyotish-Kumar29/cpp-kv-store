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
# -----------------------------------------------------------------------------

import socket
import time

SERVER_IP = "127.0.0.1"
SERVER_PORT = 8080
NUM_KEYS = 10000    # Total unique keys to verify


# A helper to track specific failure types
class Diagnostics:
    def __init__(self):
        self.crashes = 0      # Connection Refused / Broken Pipe
        self.hangs = 0        # Socket Timeout
        self.corrupt = 0      # Logic Errors (Data mismatch)
        self.missing = 0      # Logic Errors (Key not found)

    def report(self):
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
    print("\n\n" + "="*60)
    print("         TEST: DATA INTEGRITY CHECK (ACID-LITE)             ")
    print("="*60)
    print(f"Target:       {SERVER_IP}:{SERVER_PORT}")
    print(f"Keys:         {NUM_KEYS}")
    print("-" * 60)

def run_test():
    diag = Diagnostics()

    # 1. WRITE PHASE
    print_header()
    print(f"[Phase 1] Writing {NUM_KEYS} keys...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)    #  Wait max 2 seconds per request
    try:
        sock.connect((SERVER_IP, SERVER_PORT))
    except ConnectionRefusedError:
        print("ERROR: Server is not running. Start ./build/kvstore first.")
        return
    
    start_time = time.time()

    for i in range(NUM_KEYS):
        # Deterministic Key/Value generation
        # We know exactly what 'check_500' should contain ('value_500')
        key = f"check_{i}"
        val = f"value_{i}"
        
        command = f"SET {key} {val}\n"

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
                diag.corrupt += 1   # Update diag
        
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

    # Check if we actually finished or crashed
    if diag.crashes > 0 or diag.hangs > 0:
        print(f"Write Phase ABORTED after {i} keys ({write_time:.2f}s).")
    else:
        print(f"Write Phase Complete ({write_time:.2f}s).")

    # Early exit if write failed
    if diag.crashes > 0 or diag.hangs > 0:
        diag.report()
        return
    
    # 2. READ & VERIFY PHASE
    print(f"Reading back {NUM_KEYS} keys to verify...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    sock.connect((SERVER_IP, SERVER_PORT))
    
    
    for i in range(NUM_KEYS):
        key = f"check_{i}"
        expected_val = f"value_{i}"
        command = f"GET {key}\n"
        
        try:  
            sock.sendall(command.encode())
            response = sock.recv(1024).decode().strip()

            if response == "NOT FOUND":
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

    # 3. INVALID COMMANDS PHASE
    print(f"Testing invalid commands and parser errors...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    sock.connect((SERVER_IP, SERVER_PORT))
    
    invalid_cmds = ["SET_MISSING_VALUE key\n", "GET key with space\n", "UNKNOWN key\n", "BLAH\n"]
    for cmd in invalid_cmds:
        try:
            sock.sendall(cmd.encode())
            response = sock.recv(1024).decode().strip()
            if "ERROR - Invalid or unknown command" not in response:
                print(f"Parser Error logic failed. Sent '{cmd.strip()}', got '{response}'")
                diag.corrupt += 1
        except Exception as e:
            diag.crashes += 1
    sock.close()
    diag.report()

if __name__ == "__main__":
    run_test()