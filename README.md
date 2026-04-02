# messageEngine

![CI](https://github.com/becool2018/messageEngine/actions/workflows/CI.yml/badge.svg)

A safety-critical C++ networking library that models realistic communication problems — latency, jitter, packet loss, duplication, reordering, and link partitions — while providing consistent, reusable messaging utilities across three transport backends (TCP, UDP, and an in-process simulation harness).

All production code complies with **JPL Power of 10**, **MISRA C++:2023**, and an **F-Prime style subset**: no exceptions, no STL, no templates, no RTTI, and no dynamic allocation on critical paths after initialization.

The library is maintained to **NASA-STD-8719.13C** and **NASA-STD-8739.8A** software assurance standards at **Class C** classification (infrastructure / networking library). Testing discipline voluntarily targets **Class B** rigor: all safety-critical functions require branch coverage, mandatory peer inspection (M1), and static analysis (M2), with MC/DC coverage as the goal for the five highest-hazard functions.

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
9. [Using the Library](#using-the-library)
10. [Use Cases](#use-cases)
11. [Safety & Assurance Documents](#safety--assurance-documents)
12. [Coding Standards](#coding-standards)
13. [Project Standards Files](#project-standards-files)
14. [Claude Skills](#claude-skills)
15. [Code Statistics](#code-statistics)

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

These are **application-layer** semantics implemented entirely in `DeliveryEngine` (`src/core/`). No transport backend is aware of the reliability class — they all just send and receive serialized bytes. Over TCP, transport-level loss cannot occur (TCP guarantees delivery), so BEST_EFFORT will never drop and RELIABLE_RETRY will never need to retransmit due to network loss. However, the application-level ACK handshake, retry scheduler, and duplicate suppression all still execute regardless of transport, because they guard against application-level failures (peer crash, logic errors, expired messages) rather than network-level loss.

### 3. Serialization
- Deterministic big-endian (network byte order) wire format
- Fixed 44-byte header; payload follows immediately
- Validated on deserialization: padding bytes, payload length bounds

### 4. Transport Abstraction
A single `TransportInterface` API (`init`, `send_message`, `receive_message`, `close`) hides all transport differences. Five implementations are provided:

| Backend | Description |
|---|---|
| **TcpBackend** | Connection-oriented; length-prefixed framing; multi-client server support |
| **UdpBackend** | Connectionless; one datagram per message; no built-in reliability |
| **TlsTcpBackend** | TLS-encrypted TCP (mbedTLS 4.0 PSA Crypto); per-client TLS context; plaintext fallback for tests |
| **DtlsUdpBackend** | DTLS-encrypted UDP; DTLS cookie anti-replay; MTU enforcement (1,400 bytes); plaintext fallback |
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
│  MessageEnvelope · Timestamp · MessageId              │
│  Logger · Assert · AssertState                        │
│  IResetHandler · AbortResetHandler                    │
│  TransportInterface · ChannelConfig · TlsConfig       │
│  ImpairmentConfig · Types                             │
└──────────────────────┬───────────────────────────────┘
                       │  implements TransportInterface
┌──────────────────────▼───────────────────────────────┐
│  Platform Layer  (src/platform/)                      │
│  TcpBackend · UdpBackend · LocalSimHarness            │
│  TlsTcpBackend · DtlsUdpBackend                       │
│  ImpairmentEngine · ImpairmentConfigLoader            │
│  PrngEngine · SocketUtils                             │
│  ISocketOps · SocketOpsImpl                           │
│  IMbedtlsOps · MbedtlsOpsImpl                        │
└──────────────────────┬───────────────────────────────┘
                       │  POSIX sockets / OS APIs / mbedTLS
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
│   │   ├── TlsConfig.hpp
│   │   ├── RingBuffer.hpp
│   │   ├── ImpairmentConfig.hpp
│   │   ├── Serializer.hpp/cpp
│   │   ├── MessageId.hpp/cpp
│   │   ├── Timestamp.hpp/cpp
│   │   ├── DuplicateFilter.hpp/cpp
│   │   ├── AckTracker.hpp/cpp
│   │   ├── RetryManager.hpp/cpp
│   │   ├── DeliveryEngine.hpp/cpp
│   │   ├── Assert.hpp            # NEVER_COMPILED_OUT_ASSERT macro
│   │   ├── AssertState.hpp/cpp   # Global assert handler registry
│   │   ├── IResetHandler.hpp     # Abstract fatal-assert handler interface
│   │   ├── AbortResetHandler.hpp # POSIX abort() implementation
│   │   └── Logger.hpp
│   ├── platform/         # OS and socket adapters
│   │   ├── PrngEngine.hpp/cpp
│   │   ├── ImpairmentConfig.hpp
│   │   ├── ImpairmentEngine.hpp/cpp
│   │   ├── ImpairmentConfigLoader.hpp/cpp  # INI-style file parser
│   │   ├── ISocketOps.hpp                  # Injectable POSIX socket interface
│   │   ├── SocketOpsImpl.hpp/cpp           # Production POSIX implementation
│   │   ├── IMbedtlsOps.hpp                 # Injectable mbedTLS interface
│   │   ├── MbedtlsOpsImpl.hpp/cpp          # Production mbedTLS implementation
│   │   ├── SocketUtils.hpp/cpp
│   │   ├── TcpBackend.hpp/cpp
│   │   ├── UdpBackend.hpp/cpp
│   │   ├── TlsTcpBackend.hpp/cpp           # TLS over TCP (mbedTLS 4.0 PSA)
│   │   ├── DtlsUdpBackend.hpp/cpp          # DTLS over UDP (mbedTLS 4.0 PSA)
│   │   └── LocalSimHarness.hpp/cpp
│   └── app/              # Demo programs
│       ├── Server.cpp
│       └── Client.cpp
├── tests/                # Unit tests (one binary per module)
│   ├── MockSocketOps.hpp           # Injectable ISocketOps stub for TcpBackend/UdpBackend tests
│   ├── test_MessageEnvelope.cpp
│   ├── test_Serializer.cpp
│   ├── test_DuplicateFilter.cpp
│   ├── test_AckTracker.cpp
│   ├── test_RetryManager.cpp
│   ├── test_DeliveryEngine.cpp
│   ├── test_ImpairmentEngine.cpp
│   ├── test_ImpairmentConfigLoader.cpp
│   ├── test_LocalSim.cpp
│   ├── test_AssertState.cpp
│   ├── test_SocketUtils.cpp
│   ├── test_TcpBackend.cpp
│   ├── test_UdpBackend.cpp
│   ├── test_TlsTcpBackend.cpp
│   └── test_DtlsUdpBackend.cpp
├── docs/                 # Requirements, design, and analysis documents
├── Makefile
├── .cppcheck-suppress
├── .gitignore
├── CLAUDE.md             # Project-specific requirements and standards
└── .claude/CLAUDE.md     # Global C/C++ coding standards
```

---

## Building

### Required Tools

| Tool | Minimum Version | Purpose |
|---|---|---|
| **GCC** or **Clang** | GCC 8+ / Clang 7+ | C++17 compiler; must support `-std=c++17 -fno-exceptions -fno-rtti` |
| **GNU Make** | 3.81+ | Build system; no CMake required |
| **pthreads** | POSIX | TCP/UDP receiver threads; linked via `-lpthread` |
| **mbedTLS** | 3.x / 4.x | TLS and DTLS backends (`TlsTcpBackend`, `DtlsUdpBackend`); located via `pkg-config` with Homebrew fallback |

### Optional Tools (static analysis and coverage)

| Tool | Purpose | Install |
|---|---|---|
| **Clang-Tidy** | Tier 2a static analysis (`make lint`) | `brew install llvm` / `apt install clang-tidy` |
| **Cppcheck** | Tier 2b MISRA checking (`make cppcheck`) | `brew install cppcheck` / `apt install cppcheck` |
| **scan-build** (LLVM) | Tier 2c path-sensitive analysis (`make scan_build`) | Included with LLVM (`brew install llvm`) |
| **llvm-profdata / llvm-cov** | Branch coverage (`make coverage`) | Included with LLVM (`brew install llvm`) |

> **macOS note:** The Makefile auto-detects LLVM tools at `/opt/homebrew/opt/llvm/bin/` and falls back to PATH if not found there. No manual configuration is needed after `brew install llvm` unless you have a non-standard Homebrew prefix.

### Installing Prerequisites

**macOS (Homebrew):**
```bash
brew install gcc make llvm cppcheck pkg-config mbedtls
```

**Ubuntu / Debian:**
```bash
sudo apt update
sudo apt install build-essential clang clang-tidy clang-tools cppcheck llvm pkg-config libmbedtls-dev
```

> **Linux note:** `clang-tools` provides `scan-build`. All LLVM tools fall back to PATH automatically. The `scan-build` analyzer path is resolved via `which clang` at build time; no manual path configuration is needed.

### Building

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
build/test_ImpairmentConfigLoader
build/test_LocalSim
build/test_AssertState
build/test_SocketUtils
build/test_TcpBackend
build/test_UdpBackend
build/test_TlsTcpBackend
build/test_DtlsUdpBackend
```

---

## Coverage Analysis

Uses LLVM source-based coverage (`clang++ -fprofile-instr-generate -fcoverage-mapping`).

```bash
# Build instrumented binaries, run all tests, print per-file summary
make coverage

# Full report: per-file summary + per-function breakdown + policy compliance table
make coverage_report

# Annotated line-level output with branch hit counts (requires make coverage first)
make coverage_show
```

Policy floor: 100% of all reachable branches for SC files (CLAUDE.md §14). Per-file ceilings below 100% are documented in `docs/COVERAGE_CEILINGS.md`; every missed branch is individually justified.

| File | Branch % | SC? | Status |
|---|---|---|---|
| `core/Serializer.cpp` | 74.36% | SC | Ceiling — 20 missed assert `[[noreturn]]` branches |
| `core/DuplicateFilter.cpp` | 76.19% | SC | Pass |
| `core/AckTracker.cpp` | 76.71% | SC | Pass |
| `core/RetryManager.cpp` | 79.12% | SC | Pass |
| `core/DeliveryEngine.cpp` | 75.22% | SC | Pass |
| `core/AssertState.cpp` | 50.00% | NSC-infra | Ceiling — abort() False branch untestable |
| `platform/ImpairmentEngine.cpp` | 74.19% | SC | Ceiling — 40 assert + 8 unreachable branches |
| `platform/ImpairmentConfigLoader.cpp` | 82.50% | SC | Ceiling — 23 assert + 5 unreachable branches |
| `platform/TcpBackend.cpp` | 77.73% | SC | Ceiling — 38 assert + 15 unreachable branches |
| `platform/UdpBackend.cpp` | 75.51% | SC | Pass |
| `platform/TlsTcpBackend.cpp` | 76.25% | SC | Pass |
| `platform/DtlsUdpBackend.cpp` | 81.76% | SC | Ceiling — 35 assert + 19 unreachable branches |
| `platform/LocalSimHarness.cpp` | 72.46% | SC | Ceiling — 17 assert + 2 unreachable branches |
| `platform/MbedtlsOpsImpl.cpp` | 69.77% | SC | Ceiling — 26 assert `[[noreturn]]` branches |
| `platform/SocketUtils.cpp` | 64.94% | NSC | Ceiling — POSIX errors unreachable on loopback |
| `platform/SocketOpsImpl.cpp` | 68.57% | NSC | Line coverage sufficient |

Ceiling files are at the maximum achievable coverage: `NEVER_COMPILED_OUT_ASSERT` generates one permanently-missed branch per call (the `[[noreturn]]` `abort()` path), and certain POSIX/mbedTLS error paths cannot be triggered in a loopback test environment. All are documented deviations, not defects.

---

## Static Analysis

Three tiers of static analysis are configured in the Makefile (see `docs/STATIC_ANALYSIS_TOOLCHAIN.md`):

| Tier | Tool | Status | Target |
|---|---|---|---|
| **1** | Compiler `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` | Active — enforced on every build | `make all` |
| **2a** | Clang-Tidy (strict for `src/`, relaxed for `tests/`) | Active | `make lint` |
| **2b** | Cppcheck flow/style analysis (CI-safe; no MISRA addon) | Active | `make cppcheck` |
| **2b+** | Cppcheck + MISRA C++:2023 addon (macOS only) | Active locally | `make cppcheck-misra` |
| **2c** | Clang Static Analyzer — path-sensitive, alpha checkers | Active | `make scan_build` |
| **3** | PC-lint Plus — formal MISRA C++:2023 compliance report | Pending licence | `make pclint` |

```bash
# Run all active Tier 2 tools in sequence (CI target)
make static_analysis

# Individual targets
make lint
make cppcheck
make cppcheck-misra   # macOS only — Ubuntu cppcheck 2.13 does not support --addon=misra
make scan_build
make pclint           # Tier 3 — requires PC-lint Plus licence and pclint/ config directory
```

Documented suppressions live in `.cppcheck-suppress`; each entry requires a justification comment and a `DEFECT_LOG.md` reference. On Linux, comment lines are stripped from the suppression file before passing to cppcheck (Ubuntu 24.04 cppcheck 2.13 limitation).

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

## Using the Library

This section shows how to integrate messageEngine into your own application. The public API surface is small: a `TransportConfig`, a `ChannelConfig`, a transport backend, and the `DeliveryEngine`.

### 1. Choose a transport backend

| Backend | Header | When to use |
|---|---|---|
| `TcpBackend` | `platform/TcpBackend.hpp` | Production; connection-oriented, length-framed |
| `UdpBackend` | `platform/UdpBackend.hpp` | Production; datagram, low-overhead |
| `TlsTcpBackend` | `platform/TlsTcpBackend.hpp` | Production; TLS-encrypted TCP; set `tls_enabled=false` for plaintext test mode |
| `DtlsUdpBackend` | `platform/DtlsUdpBackend.hpp` | Production; DTLS-encrypted UDP; MTU-bounded datagrams |
| `LocalSimHarness` | `platform/LocalSimHarness.hpp` | Tests and simulation; no OS sockets |

All five implement `TransportInterface` and are interchangeable at the `DeliveryEngine` level.

### 2. Configure the transport and channel

```cpp
#include "core/ChannelConfig.hpp"
#include "core/Types.hpp"

// Start from safe defaults, then override what you need.
TransportConfig cfg;
transport_config_default(cfg);           // fills loopback TCP, port 9000

cfg.kind               = TransportKind::TCP;
cfg.is_server          = false;          // true for the listening side
cfg.local_node_id      = 2U;            // must be non-zero; unique per node
cfg.peer_port          = 9000U;
cfg.num_channels       = 1U;

// Configure channel 0
channel_config_default(cfg.channels[0], 0U);
cfg.channels[0].reliability    = ReliabilityClass::RELIABLE_RETRY;
cfg.channels[0].max_retries    = 3U;
cfg.channels[0].retry_backoff_ms = 200U; // initial interval; doubles each attempt
cfg.channels[0].recv_timeout_ms  = 500U;
```

Key enumerations:

| Type | Values |
|---|---|
| `ReliabilityClass` | `BEST_EFFORT`, `RELIABLE_ACK`, `RELIABLE_RETRY` |
| `OrderingMode` | `ORDERED`, `UNORDERED` |
| `TransportKind` | `TCP`, `UDP`, `LOCAL_SIM`, `DTLS_UDP` |

### 3. Initialize the backend and DeliveryEngine

```cpp
#include "platform/TcpBackend.hpp"
#include "core/DeliveryEngine.hpp"

TcpBackend transport;
Result res = transport.init(cfg);
if (!result_ok(res)) {
    // handle fatal error — transport not available
}

DeliveryEngine engine;
engine.init(&transport, cfg.channels[0], cfg.local_node_id);
```

`init()` is the only point where any allocation takes place. All state is fixed-size and stack/statically allocated.

### 4. Send a message

```cpp
#include "core/MessageEnvelope.hpp"
#include "core/Timestamp.hpp"

uint64_t now_us = timestamp_now_us();

MessageEnvelope env;
envelope_init(env);                          // zero-fills the struct

env.message_type      = MessageType::DATA;
env.destination_id    = 1U;                  // target node ID
env.priority          = 0U;                  // 0 = highest
env.reliability_class = ReliabilityClass::RELIABLE_RETRY;
env.timestamp_us      = now_us;
env.expiry_time_us    = timestamp_deadline_us(now_us, 5000U); // 5-second TTL

// Copy payload (up to MSG_MAX_PAYLOAD_BYTES = 4096)
const char* msg = "Hello";
(void)memcpy(env.payload, msg, 5U);
env.payload_length = 5U;

Result res = engine.send(env, now_us);
// engine.send() fills env.message_id and env.source_id automatically
```

### 5. Receive a message

```cpp
MessageEnvelope reply;
envelope_init(reply);

Result res = engine.receive(reply, 500U /*timeout_ms*/, timestamp_now_us());
if (result_ok(res) && envelope_is_data(reply)) {
    // process reply.payload[0..reply.payload_length-1]
}
// ERR_TIMEOUT  → no message arrived within 500 ms
// ERR_EXPIRED  → message arrived but was past its expiry_time_us
// ERR_DUPLICATE → silently deduped (RELIABLE_RETRY only)
```

`DeliveryEngine::receive()` handles ACKs, duplicate filtering, and expiry checks transparently. Only `DATA` messages reach the caller.

### 6. Service the engine periodically

For `RELIABLE_RETRY` and `RELIABLE_ACK` modes, call these on every iteration of your event loop:

```cpp
uint64_t now_us = timestamp_now_us();
uint32_t retried  = engine.pump_retries(now_us);       // re-sends due messages
uint32_t timeouts = engine.sweep_ack_timeouts(now_us); // logs missed ACKs
```

### 7. Shutdown

```cpp
transport.close();
```

---

### Testing with LocalSimHarness

`LocalSimHarness` runs entirely in-process — no sockets, no threads, deterministic. Link two instances together and they exchange messages through a `RingBuffer`.

```cpp
#include "platform/LocalSimHarness.hpp"

LocalSimHarness node_a;
LocalSimHarness node_b;

TransportConfig cfg_a, cfg_b;
transport_config_default(cfg_a);
transport_config_default(cfg_b);
cfg_a.local_node_id = 1U;
cfg_b.local_node_id = 2U;

(void)node_a.init(cfg_a);
(void)node_b.init(cfg_b);

node_a.link(&node_b);   // messages sent from a arrive in b's queue
node_b.link(&node_a);   // messages sent from b arrive in a's queue

DeliveryEngine engine_a, engine_b;
engine_a.init(&node_a, cfg_a.channels[0], 1U);
engine_b.init(&node_b, cfg_b.channels[0], 2U);
```

---

### Injecting Network Faults (ImpairmentEngine)

Every backend (`LocalSimHarness`, `TcpBackend`, `UdpBackend`, `DtlsUdpBackend`) has an **internal** `ImpairmentEngine` initialized automatically from `cfg.channels[0].impairment`. Configure the `ImpairmentConfig` field on the channel before calling `init()` — no separate `ImpairmentEngine` object is needed.

```cpp
#include "core/ChannelConfig.hpp"   // ImpairmentConfig is embedded here
#include "core/ImpairmentConfig.hpp"

// Build the transport config as normal, then configure impairments:
TransportConfig cfg;
transport_config_default(cfg);
cfg.local_node_id = 1U;

ImpairmentConfig& imp = cfg.channels[0].impairment;
impairment_config_default(imp);          // all faults off, deterministic seed
imp.enabled               = true;
imp.loss_probability      = 0.10;        // 10 % packet loss
imp.fixed_latency_ms      = 20U;         // 20 ms added to every message
imp.jitter_mean_ms        = 5U;          // ±5 ms random jitter
imp.partition_enabled     = true;
imp.partition_gap_ms      = 1000U;       // link up for 1 second
imp.partition_duration_ms = 200U;        // then down for 200 ms
imp.prng_seed             = 42ULL;       // same seed → same fault sequence

// The backend initializes its internal ImpairmentEngine from cfg.channels[0].impairment
LocalSimHarness node_a;
(void)node_a.init(cfg);                  // faults active from first send_message() call
```

All impairment decisions are driven by a seedable xorshift64 PRNG — given the same seed and input sequence, every run produces an identical fault pattern.

---

### Error Handling

All functions return a `Result` enum. Never ignore a return value.

| Code | Meaning |
|---|---|
| `OK` | Success |
| `ERR_TIMEOUT` | No message arrived within the timeout |
| `ERR_FULL` | Send queue or delay buffer is full |
| `ERR_EMPTY` | Receive queue is empty (no message available) |
| `ERR_IO` | Socket or transport error; or link is partitioned |
| `ERR_INVALID` | Bad argument or engine not initialized |
| `ERR_EXPIRED` | Message TTL has elapsed |
| `ERR_DUPLICATE` | Message was already seen (silently filtered) |
| `ERR_OVERRUN` | Internal buffer overrun |

---

## Use Cases

The [`docs/use_cases/`](docs/use_cases/) directory contains detailed use case documents that trace every user-facing capability and system-internal sub-function through the live source code.

- **[HIGH_LEVEL_USE_CASES.md](docs/use_cases/HIGH_LEVEL_USE_CASES.md)** — index of all 42 use cases, grouped by high-level capability (HL-1 through HL-20), Application Workflow patterns, and System Internal sub-functions.

Each individual `UC_*.md` document follows a 15-section flow-of-control format covering: entry points, end-to-end control flow, call tree, branching logic, concurrency behavior, memory ownership, error handling, external interactions, state changes, and known risks.

---

## Safety & Assurance Documents

NASA-STD-8719.13C / NASA-STD-8739.8A compliance artifacts maintained in [`docs/`](docs/):

| Document | Description |
|---|---|
| [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md) | Software Safety Hazard Analysis (HAZ-001–HAZ-007), Failure Mode and Effects Analysis (FMEA) for every major component, and Safety-Critical (SC) vs Non-Safety-Critical (NSC) classification for every public function in `src/`. |
| [STATE_MACHINES.md](docs/STATE_MACHINES.md) | Formal state-transition tables and invariants for the three safety-critical state machines: `AckTracker`, `RetryManager`, and `ImpairmentEngine` (including the partition sub-state). |
| [TRACEABILITY_MATRIX.md](docs/TRACEABILITY_MATRIX.md) | Bidirectional requirements traceability matrix mapping every `[REQ-x.x]` ID from `CLAUDE.md` to the `src/` file that implements it and the `tests/` file that verifies it. |
| [STACK_ANALYSIS.md](docs/STACK_ANALYSIS.md) | Worst-case stack depth analysis across four call chains; worst-case frame depth is 10 frames (retry pump), worst-case stack size is ~748 bytes (outbound send). |
| [WCET_ANALYSIS.md](docs/WCET_ANALYSIS.md) | Worst-case execution time analysis expressed as closed-form operation counts for every SC function, derived from the compile-time capacity constants in `src/core/Types.hpp`. |
| [MCDC_ANALYSIS.md](docs/MCDC_ANALYSIS.md) | MC/DC coverage analysis for the five highest-hazard SC functions: `DeliveryEngine::send`, `DeliveryEngine::receive`, `DuplicateFilter::check_and_record`, `Serializer::serialize`, `Serializer::deserialize`. |
| [INSPECTION_CHECKLIST.md](docs/INSPECTION_CHECKLIST.md) | Moderator-led formal inspection checklist (NPR 7150.2D §3 / NASA-STD-8739.8); entry/exit criteria, severity definitions, and waiver policy for all `src/` changes. |
| [DEFECT_LOG.md](docs/DEFECT_LOG.md) | Cumulative inspection defect record; every defect found during formal review of `src/` changes is logged here with disposition and sign-off. |

---

## Coding Standards

All production code (`src/`) is written to the following standards. Deviations require an in-code comment with justification.

**Software classification:** NASA-STD-8719.13C / NASA-STD-8739.8A **Class C** (infrastructure / networking library). **Verification discipline voluntarily targets Class B**: all safety-critical functions require branch coverage (M4), mandatory peer inspection (M1), and static analysis (M2); MC/DC coverage (M5) is the goal for the five highest-hazard functions. See `docs/HAZARD_ANALYSIS.md` and `docs/MCDC_ANALYSIS.md`.

| Standard | Requirement |
|---|---|
| **JPL Power of 10** | No goto, no recursion; fixed loop bounds; no dynamic allocation after init; cyclomatic complexity ≤ 10 per function (enforced by `make lint`); ≥ 2 assertions per function; minimal variable scope; all return values checked; minimal preprocessor use; ≤ 1 pointer indirection; zero compiler warnings |
| **MISRA C++:2023** | Required rules enforced; advisory rules followed; all deviations documented |
| **F-Prime subset** | ISO C++17; `-fno-exceptions`; `-fno-rtti`; no STL (except `std::atomic<T>`); no templates; no explicit function pointers (vtable-backed virtual dispatch is a documented exception); `std::atomic<T>` is a documented carve-out |
| **Error handling** | All errors returned via `Result` enum (`OK`, `ERR_TIMEOUT`, `ERR_FULL`, `ERR_IO`, …); no exceptions |
| **Assertions** | `NEVER_COMPILED_OUT_ASSERT(cond)` — always compiled in; in debug/test builds calls `abort()`; in production logs FATAL and triggers a controlled reset |
| **Layering** | App → Core → Platform → OS; no upward dependencies; no cyclic dependencies |

---

## Project Standards Files

Two CLAUDE.md files govern all code in this repository and divide responsibility cleanly:

| File | Purpose |
|---|---|
| `.claude/CLAUDE.md` | **Global C/C++ coding standard.** Contains all 10 JPL Power of 10 rules, MISRA C++:2023 compliance requirements, F-Prime style subset, architecture/layering rules, security posture, and NASA assurance mindset. Also cross-references `.claude/VERIFICATION_POLICY.md` and `CLAUDE.md` §§11/13 for traceability and safety obligations. |
| `CLAUDE.md` | **Project-specific spec.** Contains everything tied to this repository: numbered application requirements (`[REQ-x.x]`), references to `docs/` artifacts, the per-directory rule compliance table, named static analysis toolchain, `NEVER_COMPILED_OUT_ASSERT` policy, traceability rules, formal inspection process, and safety/coverage/WCET/formal-methods obligations. |
| `.claude/VERIFICATION_POLICY.md` | **Verification policy (VVP-001).** Defines the minimum verification methods (M1–M7) required at each software classification level (Class C / B / A) and the rules governing architectural ceiling arguments, fault injection, and injectable interface requirements. |

**How they divide responsibility:** `.claude/CLAUDE.md` is the authoritative coding standard; `CLAUDE.md` is the project-specific spec (requirement IDs, file paths, process rules, safety artifacts). `VERIFICATION_POLICY.md` governs what test evidence is required at each classification level.

---

## Claude Skills

This project ships a set of reusable Claude Code skills in `.claude/skills/`. Skills are invoked with a `/` prefix from within a Claude Code session.

| Skill | Invocation | Description |
|---|---|---|
| **read-standards** | `/read-standards` | Reads and internalizes both `CLAUDE.md` and `.claude/CLAUDE.md` in full, then confirms understanding of Power of 10, MISRA C++:2023, layering rules, safety annotations, and traceability policy. Run this at the start of any session that involves writing or reviewing C/C++ code. |
| **generate_use_case_list** | `/generate_use_case_list` | Reads the live source code in `src/` (headers, implementations, and app entry points) and all test files in `tests/` to regenerate `docs/use_cases/HIGH_LEVEL_USE_CASES.md` from scratch. Automatically picks up new files and ignores deleted ones. Classifies each capability as a user-facing HL group, an Application Workflow pattern, or a System Internal sub-function using a concrete decision algorithm anchored to what `src/app/` code actually calls. |
| **generate_use_cases** | `/generate_use_cases` | Reads `HIGH_LEVEL_USE_CASES.md` and all source files, then regenerates every individual `UC_*.md` document in `docs/use_cases/` using the 15-section flow-of-control format defined in `docs/use_cases/use_case_format.txt`. Overwrites existing UC files; creates new ones for any UC number that has no matching file. |
| **find_missing_use_cases** | `/find_missing_use_cases` | Compares the live source code against existing `UC_*.md` files and `HIGH_LEVEL_USE_CASES.md` to identify undocumented capabilities. Writes a new `UC_*.md` for each gap (using the same 15-section format) and inserts the new entries into `HIGH_LEVEL_USE_CASES.md`. Never modifies existing UC files. |

### Directory layout

```
.claude/
└── skills/
    ├── read-standards/
    │   └── SKILL.md
    ├── generate_use_case_list/
    │   └── SKILL.md
    ├── generate_use_cases/
    │   └── SKILL.md
    └── find_missing_use_cases/
        └── SKILL.md
```

### Adding a new skill

1. Create a subdirectory under `.claude/skills/<skill-name>/`.
2. Add a `SKILL.md` file with the required YAML front matter:
   ```yaml
   ---
   name: <skill-name>
   description: <one-line description shown in Claude's skill list>
   user-invocable: true
   allowed-tools: <comma-separated list, e.g. Read, Bash, Edit>
   ---
   ```
3. Write the skill prompt body below the front matter — this is what Claude executes when the skill is invoked.
4. Reference the new skill in this table.

---

## Code Statistics

| Category | Lines |
|---|---|
| `src/` (production + headers) | 9,578 |
| `tests/` | 11,017 |
| **Total** | **20,595** |

54 source files across 3 layers and 15 test suites.
