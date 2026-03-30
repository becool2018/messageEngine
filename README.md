# messageEngine

A safety-critical C++ networking library that models realistic communication problems — latency, jitter, packet loss, duplication, reordering, and link partitions — while providing consistent, reusable messaging utilities across three transport backends (TCP, UDP, and an in-process simulation harness).

All production code complies with **JPL Power of 10**, **MISRA C++:2023**, and an **F-Prime style subset**: no exceptions, no STL, no templates, no RTTI, and no dynamic allocation on critical paths after initialization.

---

## Table of Contents

1. [Requirements Overview](#requirements-overview)
2. [Architecture](#architecture)
3. [Directory Structure](#directory-structure)
4. [Building](#building)
5. [Running the Tests](#running-the-tests)
6. [Coverage Analysis](#coverage-analysis)
7. [Static Analysis](#static-analysis)
8. [Running the Demo (Server / Client)](#running-the-demo-server--client)
9. [Coding Standards](#coding-standards)
10. [Code Statistics](#code-statistics)

---

## Requirements Overview

### 1. Message Envelope
Every message is carried in a standard `MessageEnvelope` containing:
- **Message type** — DATA, ACK, NAK, or HEARTBEAT
- **Unique message ID** — 64-bit, monotonically increasing, never zero
- **Timestamps** — creation time and expiry time (microsecond resolution)
- **Node addressing** — `source_id` and `destination_id`
- **Priority and reliability class** — BEST_EFFORT, RELIABLE_ACK, or RELIABLE_RETRY
- **Opaque payload** — up to 4,096 bytes

### 2. Delivery Semantics
The system supports three delivery modes:

| Mode | Behavior |
|---|---|
| **Best Effort** | Fire-and-forget; no ACK, no retry; silent drop on loss |
| **Reliable with ACK** | Single send; expects one ACK; missing ACK is a failure |
| **Reliable with Retry** | Retransmits with exponential backoff until ACK or expiry; receiver deduplicates |

Expired messages (past `expiry_time`) are dropped before delivery at both sender and receiver.

### 3. Serialization
- Deterministic big-endian (network byte order) wire format
- Fixed 44-byte header; payload follows immediately
- Validated on deserialization: padding bytes, payload length bounds

### 4. Transport Abstraction
A single `TransportInterface` API (`init`, `send_message`, `receive_message`, `close`) hides all transport differences. Three implementations are provided:

| Backend | Description |
|---|---|
| **TcpBackend** | Connection-oriented; length-prefixed framing; multi-client server support |
| **UdpBackend** | Connectionless; one datagram per message; no built-in reliability |
| **LocalSimHarness** | In-process; two instances linked by pointer; no OS sockets; deterministic |

### 5. Impairment Engine
A configurable network fault injection layer sits between the transport abstraction and the underlying I/O. Supported impairments (all independently togglable):

| Impairment | Description |
|---|---|
| **Fixed latency** | Adds a constant delay to every message |
| **Jitter** | Adds random variable delay around a configured mean |
| **Packet loss** | Drops messages with a configurable probability |
| **Duplication** | Queues an extra copy of a message with a short time offset |
| **Reordering** | Buffers a window of messages and releases them out of order |
| **Partition** | Drops all traffic for a configured duration, then resumes |

All impairment decisions are driven by a seedable xorshift64 PRNG — identical seed and input produces an identical fault sequence every run.

### 6. Reliability Helpers
- **Duplicate filter** — sliding window of 128 `(source_id, message_id)` pairs; evicts the oldest on overflow
- **ACK tracker** — 32-slot fixed table; tracks pending ACKs with deadlines; sweeps timed-out entries
- **Retry manager** — per-message exponential backoff scheduler; doubles interval each attempt, capped at 60 s; cancelled on ACK

### 7. Observability
- Severity-tagged logging (`INFO`, `WARNING_LO`, `WARNING_HI`, `FATAL`) to stderr via a fixed 512-byte stack buffer
- No heap allocation in the logger

### 8. Determinism and Testability
- All components are testable without real network hardware via `LocalSimHarness`
- `ImpairmentEngine` is fully reproducible given the same seed
- External dependencies are injectable via `TransportInterface` (pointer passed at `init`)

---

## Architecture

Dependencies flow strictly downward. No lower layer may reference a higher one; cyclic dependencies are prohibited.

```
┌──────────────────────────────────────────────────────┐
│           App Layer  (src/app/)                       │
│           Server · Client                             │
└──────────────────────┬───────────────────────────────┘
                       │  uses
┌──────────────────────▼───────────────────────────────┐
│  Core Layer  (src/core/)                              │
│  DeliveryEngine · Serializer · AckTracker             │
│  RetryManager · DuplicateFilter · RingBuffer          │
│  MessageEnvelope · Timestamp · MessageIdGen           │
└──────────────────────┬───────────────────────────────┘
                       │  implements TransportInterface
┌──────────────────────▼───────────────────────────────┐
│  Platform Layer  (src/platform/)                      │
│  TcpBackend · UdpBackend · LocalSimHarness            │
│  ImpairmentEngine · PrngEngine · SocketUtils          │
└──────────────────────┬───────────────────────────────┘
                       │  POSIX sockets / OS APIs
┌──────────────────────▼───────────────────────────────┐
│  OS  (Berkeley sockets, POSIX clocks, pthreads)       │
└──────────────────────────────────────────────────────┘
```

---

## Directory Structure

```
messageEngine/
├── src/
│   ├── core/             # Transport-agnostic message layer
│   │   ├── Types.hpp         # Enums, constants, NodeId
│   │   ├── MessageEnvelope.hpp
│   │   ├── TransportInterface.hpp
│   │   ├── ChannelConfig.hpp
│   │   ├── RingBuffer.hpp
│   │   ├── Serializer.hpp/cpp
│   │   ├── MessageId.hpp/cpp
│   │   ├── Timestamp.hpp/cpp
│   │   ├── DuplicateFilter.hpp/cpp
│   │   ├── AckTracker.hpp/cpp
│   │   ├── RetryManager.hpp/cpp
│   │   ├── DeliveryEngine.hpp/cpp
│   │   ├── Assert.hpp
│   │   └── Logger.hpp
│   ├── platform/         # OS and socket adapters
│   │   ├── PrngEngine.hpp/cpp
│   │   ├── ImpairmentConfig.hpp
│   │   ├── ImpairmentEngine.hpp/cpp
│   │   ├── SocketUtils.hpp/cpp
│   │   ├── TcpBackend.hpp/cpp
│   │   ├── UdpBackend.hpp/cpp
│   │   └── LocalSimHarness.hpp/cpp
│   └── app/              # Demo programs
│       ├── Server.cpp
│       └── Client.cpp
├── tests/                # Unit tests (one binary per module)
│   ├── test_MessageEnvelope.cpp
│   ├── test_Serializer.cpp
│   ├── test_DuplicateFilter.cpp
│   ├── test_AckTracker.cpp
│   ├── test_RetryManager.cpp
│   ├── test_DeliveryEngine.cpp
│   ├── test_ImpairmentEngine.cpp
│   └── test_LocalSim.cpp
├── docs/                 # Requirements, design, and analysis documents
├── Makefile
├── .cppcheck-suppress
├── .gitignore
├── CLAUDE.md             # Project-specific requirements and standards
└── .claude/CLAUDE.md     # Global C/C++ coding standards
```

---

## Building

**Prerequisites:** GCC with C++17 support, GNU Make, pthreads.

```bash
# Build everything (server, client, all test binaries)
make all

# Build individual targets
make server
make client
make tests
```

The Makefile enforces the zero-warnings policy: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`.

---

## Running the Tests

```bash
# Build and run all test suites
make run_tests
```

Expected output: every test prints `PASS: <test_name>` and the suite ends with `=== ALL TESTS PASSED ===`.

Individual test binaries can also be run directly:

```bash
build/test_MessageEnvelope
build/test_Serializer
build/test_DuplicateFilter
build/test_AckTracker
build/test_RetryManager
build/test_DeliveryEngine
build/test_ImpairmentEngine
build/test_LocalSim
```

---

## Coverage Analysis

Uses LLVM source-based coverage (`clang++ -fprofile-instr-generate -fcoverage-mapping`).

```bash
# Build instrumented binaries, run all tests, and print branch coverage report
make coverage

# Print annotated line-level output (requires make coverage first)
make coverage_show
```

Current branch coverage for key modules:

| Module | Branch Coverage |
|---|---|
| `Serializer.cpp` | 74.36% *(architectural ceiling — 20 NEVER_COMPILED_OUT_ASSERT fire-branches)* |
| `AckTracker.cpp` | 76.71% |
| `RetryManager.cpp` | 75.53% |
| `DeliveryEngine.cpp` | 75.22% |
| `ImpairmentEngine.cpp` | 68.82% |
| `LocalSimHarness.cpp` | 69.01% |

---

## Static Analysis

Three tiers of static analysis are configured in the Makefile:

```bash
# Tier 2a: Clang-Tidy (strict config for src/, relaxed for tests/)
make lint

# Tier 2b: Cppcheck with MISRA C++:2023 addon
make cppcheck

# Tier 2c: Clang Static Analyzer (path-sensitive, alpha checkers enabled)
make scan_build

# Run all Tier 2 tools in sequence (CI target)
make static_analysis
```

Documented suppressions live in `.cppcheck-suppress`; each entry requires a justification comment and a `DEFECT_LOG.md` reference.

---

## Running the Demo (Server / Client)

```bash
# Terminal 1: start the echo server (default port 9000)
build/server

# Terminal 2: connect the client and send five messages
build/client

# Optional: override port
build/server 9001
build/client 127.0.0.1 9001
```

The client sends five `RELIABLE_RETRY` messages and prints each echo reply. The server logs all received messages and echoes them back.

---

## Coding Standards

All production code (`src/`) is written to the following standards. Deviations require an in-code comment with justification.

| Standard | Requirement |
|---|---|
| **JPL Power of 10** | No recursion; fixed loop bounds; no dynamic allocation after init; functions ≤ 1 page; ≥ 2 assertions per function; all return values checked; minimal preprocessor use; ≤ 1 pointer indirection; zero compiler warnings |
| **MISRA C++:2023** | Required rules enforced; advisory rules followed; all deviations documented |
| **F-Prime subset** | ISO C++17; `-fno-exceptions`; `-fno-rtti`; no STL; no templates; no function pointers; `std::atomic<T>` is a documented carve-out |
| **Error handling** | All errors returned via `Result` enum (`OK`, `ERR_TIMEOUT`, `ERR_FULL`, `ERR_IO`, …); no exceptions |
| **Assertions** | `NEVER_COMPILED_OUT_ASSERT(cond)` — always compiled in; in debug/test builds calls `abort()`; in production logs FATAL and triggers a controlled reset |
| **Layering** | App → Core → Platform → OS; no upward dependencies; no cyclic dependencies |

---

## Code Statistics

| Category | Lines |
|---|---|
| `src/` (production + headers) | 5,778 |
| `tests/` | 3,278 |
| **Total** | **9,056** |

38 source files across 3 layers and 8 test suites.
