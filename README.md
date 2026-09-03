# cpp-kv-store

[![Tests](https://github.com/Jyotish-Kumar29/cpp-kv-store/actions/workflows/tests.yml/badge.svg)](https://github.com/Jyotish-Kumar29/cpp-kv-store/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A single-node, in-memory key-value store written in C++, exposed over a custom
TCP protocol via an epoll-based event loop. Built from scratch: no external
KV or networking libraries — the storage engine, the server, the protocol
parser, and the persistence layer are all implemented directly.

Built as a deep dive into concurrent systems programming — the focus is
correctness under concurrency and network failure, not distributed-systems
scale. See [Scope & Limitations](#scope--limitations) for the specific
trade-offs made along the way.

## Features

- **In-memory storage engine** (`KVStore`) — `shared_mutex`-guarded map
  supporting concurrent reads with exclusive writes.
- **Epoll-based TCP server** (`TCPServer`) — single event loop, non-blocking
  sockets, handles partial reads/writes and request framing across arbitrary
  packet boundaries.
- **User accounts** (`UserBook`) — registration and authentication, isolated
  per-user key namespaces.
- **Custom text protocol** — see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for
  the full command grammar.
- **Append-only file (AOF) persistence** — writes/deletes are logged and
  replayed on restart.
- **Layered test suite** — unit, integration, protocol-level chaos/fuzz
  tests, concurrency and load stress tests, and microbenchmarks.
- **Sanitizer-verified concurrency** — the concurrent code paths are run
  under both AddressSanitizer+UndefinedBehaviorSanitizer and
  ThreadSanitizer as part of the standard test pipeline, not as a one-off.

## Architecture

| Component | Responsibility |
|---|---|
| `KVStore` | In-memory map; `shared_mutex` for concurrent get/set/delete |
| `UserBook` | User registration/auth; separate locking domain from `KVStore` |
| `TCPServer` | Epoll event loop; connection lifecycle, buffering, framing |
| `Utils` | Protocol tokenizing and integer parsing helpers |
| AOF layer | Append-only write log; replayed in order on startup |

## Getting Started

### Prerequisites

- CMake
- A C++ compiler with C++17 support (GCC or Clang)
- GoogleTest / GoogleMock
- Google Benchmark
- Python 3 (used by the protocol-level chaos/fuzz tests)

```bash
chmod +x scripts/build/*.sh scripts/run/*.sh
```

### Build

```bash
./scripts/build/build_server.sh          # server binary only
./scripts/build/build_tests_n_benchmarks.sh
./scripts/build/build_asan.sh            # ASan + UBSan test build
./scripts/build/build_tsan.sh            # TSan test build
./scripts/build/build_all.sh             # all of the above
```

### Run the server

```bash
./scripts/run/manage_server.sh start     # listens on port 8080
```

### Run the tests

```bash
./scripts/run/run_tests.sh               # unit + integration + chaos + stress
./scripts/run/run_asan.sh                # ASan + UBSan
./scripts/run/run_tsan.sh                # ThreadSanitizer
./scripts/run/run_benchmarks.sh          # microbenchmarks + network benchmarks
./scripts/run/run_all.sh                 # full pipeline: build, test, sanitize, benchmark
```

`run_all.sh` runs everything sequentially, including a 10-minute endurance
soak test (run twice — non-persistent and persistent) — expect it to take
the better part of an hour end to end. For day-to-day iteration,
`run_tests.sh` / `run_asan.sh` / `run_tsan.sh` individually are much faster.

## Testing & Verification

The suite is organized in layers, each catching a different class of bug:

- **Unit tests** (GoogleTest & Python) — `KVStore`, `UserBook`, `Utils`, and AOF
  persistence in isolation. 25 test cases total across these four suites
  (4 + 8 + 7 + 6), plus a 10,000-key read/write Python data-integrity pass.
- **Integration tests** — full request lifecycle through the real
  `TCPServer` over a live socket, including multi-user isolation and
  authentication flows. 9 test cases across `test_full_flow` and
  `test_server_client`.
- **Protocol chaos tests** (Python) — drip-fed single-byte delivery,
  requests fragmented across writes and across the server's read buffer
  boundary, pipelined requests, half-close handling, oversized/unterminated
  requests, and malformed/random-byte input. 31 checks across the two chaos
  scripts.
- **Disconnect handling** — abrupt disconnects mid-request and
  immediately after connecting, verified not to leave the server in a
  broken state for subsequent clients.
- **Concurrency stress** — sustained concurrent KV and user operations
  (8 KV-writer threads, 4 KV-reader threads, 4 user-registration threads at
  250 users/worker) with explicit invariant checks after every run, not just
  "it didn't crash."
- **Sanitizers** — the same concurrency-heavy tests above are re-run under
  AddressSanitizer+UndefinedBehaviorSanitizer and ThreadSanitizer. This is
  how a global-buffer-overflow in one of the chaos test's own `send()` calls
  was caught and fixed during development — the sanitizer pipeline is
  treated as a real part of the workflow, not a checkbox.

## Performance

Measured on the development machine (8 logical CPUs @ 4.4 GHz, 8 MB L3),
plugged into wall power. These numbers are a snapshot of one machine under
one set of conditions — CPU scaling, thermal throttling, and background
load all affect them, and they are not a guarantee of performance elsewhere.
Take them as a rough shape, not a spec sheet.

**Internal engine (in-process, no network, latency per call including lock
contention):**

| Operation | 1 thread | 8 threads | 32 threads | 100 threads |
|---|---|---|---|---|
| `Get` | 31.5 ns | 671 ns | 1.67 µs | 3.16 µs |
| `Set` | 592 ns | 6.93 µs | 36.0 µs | 114.7 µs |
| `Delete` | 242 ns | 4.85 µs | 20.3 µs | 87.3 µs |

`Set`/`Delete` degrade sharply under thread count because every write takes
the single `shared_mutex` exclusively — see
[Scope & Limitations](#scope--limitations).

**Whole-server latency, cold state (non-persistent, mixed 40% SET / 45% GET
/ 15% DEL workload over TCP, 8 concurrent connections, server growing from
empty):**

| Cumulative SET range | Median | p99 | p99.9 |
|---|---|---|---|
| 0–100 | 37.6 µs | 60.8 µs | 78.5 µs |
| 10,001–100,000 | 47.0 µs | 60.3 µs | 75.2 µs |
| 100,001–1,000,000 | 49.8 µs | 63.6 µs | 77.3 µs |

**Whole-server latency, warm/steady state (non-persistent, same workload,
each connection pre-seeded to a target key count before measuring):**

| Starting live keys/connection | Median | p99 | p99.9 |
|---|---|---|---|
| 100 | 37.5 µs | 171.2 µs | 736.1 µs |
| 10,000 | 28.7 µs | 64.3 µs | 212.6 µs |
| 1,000,000 | 24.7 µs | 50.2 µs | 59.8 µs |

Persistent mode (AOF writes enabled) adds roughly 10–20 µs of median latency
across both tables — see `run_benchmarks.sh` output for the full breakdown.

**Throughput and load:**

| Test | Result |
|---|---|
| Sustained throughput (100 threads, persistent connections, 1M requests, AOF disabled) | ~212,000 req/sec, 0 failures |
| Sustained throughput (same workload, AOF enabled) | ~203,000 req/sec, 0 failures |
| 20,000 concurrent client stress test (60s) | ~195,000 req/sec sustained, 0 failed clients |
| 10-minute endurance soak, non-persistent (100 threads) | 99.6M requests, 0 failures, ~166,000 avg req/sec |
| 10-minute endurance soak, persistent (100 threads) | 97.8M requests, 0 failures, ~163,000 avg req/sec |

Full test and benchmark output: [`run_all_tests.log`](./docs/tests_logs/run_all_tests.log)

## Continuous Integration

Every push and pull request runs five jobs in parallel — see
[`.github/workflows/tests.yml`](.github/workflows/tests.yml):

| Job | What it covers |
|---|---|
| `unit-tests` | GoogleTest unit suites via CTest, in-process integration tests (`test_full_flow`, `test_server_client`, `disconnect_test`), and the 10,000-key data-integrity check against a live server |
| `asan` | Offline unit tests rebuilt and run under AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | Offline unit tests + concurrency stress rebuilt and run under ThreadSanitizer |
| `stress-and-chaos` | In-process concurrency/user stress, plus the TCP chaos and malformed-protocol suites against a live server |
| `scalability-smoke` | 10,000 concurrent client connections against a live server, scaled down for a shared CI runner's file-descriptor budget |

A sixth job, `scalability-full`, runs the connection-scalability test at
full scale (tens of thousands of clients) but only on manual trigger
(`workflow_dispatch`), since it needs a larger `ulimit -n` and ephemeral
port range than the smoke test grants by default. The throughput/latency/
endurance benchmarks and the 10-minute soak test are **not** part of CI —
run numbers on shared runner hardware aren't representative of anything, so
those stay a manual `./scripts/run/run_benchmarks.sh` /
`./scripts/run/run_all.sh` step.

## Scope & Limitations

This project is intentionally scoped as a single-node, in-memory store with
a hand-rolled protocol, not a distributed or production-grade database.
Concretely, it does **not** currently have:

- Replication, clustering, or sharding — single process, single node.
- AOF compaction or snapshotting — the log grows unboundedly and is replayed
  in full on every restart.
- Key expiry / TTLs.
- A standard wire protocol (e.g. RESP) — the protocol is custom and
  documented in `docs/PROTOCOL.md`, not interoperable with existing clients.
- Fine-grained (per-key) locking — `KVStore` uses a single `shared_mutex`
  over the whole map, which is the main source of contention under heavy
  concurrent writes (visible in the internal-engine latency table above).

These are known trade-offs, not oversights — the project's focus has been
correctness under concurrency and network failure modes, verified with
sanitizers and protocol-level chaos testing, rather than throughput at scale.

## Repository Structure

```
.
├── benchmarks/     Internal + network microbenchmarks (Google Benchmark)
├── include/        Public headers (KVStore, TCPServer, UserBook, Utils)
├── src/            Implementation
├── main.cpp        Server entry point
├── scripts/
│   ├── build/       Per-configuration build scripts (server, tests, ASan, TSan)
│   └── run/         Per-configuration and full-pipeline test runners
├── tests/
│   ├── unit/        GoogleTest unit tests + Python data-integrity check
│   ├── integration/ Full-flow and server/client GoogleTest suites
│   ├── chaos/       Python protocol-chaos tests + disconnect-handling tests
│   └── stress/      Concurrency, user-registration, and connection-scale stress tests
├── docs/
│   └── PROTOCOL.md  Wire protocol specification
├── .github/
│   └── workflows/   CI pipeline (tests.yml)
└── data/            AOF persistence output (runtime-generated)
```

## License

MIT — see [LICENSE](LICENSE).