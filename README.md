# messageEngine

> **Experimental project.** This codebase is a learning and research exercise largely produced with AI assistance (Claude Code). It is not production software. APIs, behavior, and documentation may change without notice.

![CI](https://github.com/becool2018/messageEngine/actions/workflows/CI.yml/badge.svg)
![Stress Tests](https://github.com/becool2018/messageEngine/actions/workflows/stress.yml/badge.svg)

A C++ networking library for building and testing systems that must survive unreliable networks. It provides:

- **Three delivery modes** — best-effort, reliable-with-ACK, and reliable-with-retry-and-dedup — implemented at the application layer, transport-independent
- **Five transport backends** — TCP, UDP, TLS/TCP, DTLS/UDP, and an in-process simulation harness (`LocalSimHarness`) that needs no OS sockets
- **A fault injection engine** — inject latency, jitter, packet loss, duplication, reordering, and link partitions, all driven by a seedable PRNG for reproducible test runs
- **Bounded, fixed-allocation design** — no heap allocation on critical paths after init; all capacities are compile-time constants in `src/core/Types.hpp`
- **Observable by default** — severity-tagged logging, per-engine delivery counters, and an 8-kind event ring (`poll_event` / `drain_events`) for post-hoc analysis without callbacks or heap

**Quick capacity reference** ([full details →](docs/CAPACITY_REFERENCE.md))**:**

| Resource | Limit |
|---|---|
| Max payload | 4 096 bytes |
| Dedup window | 128 `(source, msg_id)` pairs — sliding window that detects and drops duplicate retransmissions |
| ACK tracker | 32 pending slots |
| Retry manager | 32 pending slots |
| Inbound ring | 64 messages per backend |
| Concurrent clients (TCP/TLS) | 8 (configurable via `MAX_TCP_CONNECTIONS` in `Types.hpp`) |
| Worst-case stack depth | ~764 bytes / 10 frames (DTLS outbound send path) |

**Written to:** JPL Power of 10 · MISRA C++:2023 · F-Prime style subset · NASA Class C (voluntary Class B test rigor). No exceptions, no templates, no RTTI. STL is excluded from production code with one deliberate exception: `std::atomic<T>` for integral types is permitted and used for shared state — it has no dynamic allocation, maps directly to hardware primitives, and is what MISRA C++:2023 endorses for lock-free concurrency. All other STL containers, algorithms, and headers are absent from `src/`.

> **Standards note:** MISRA C++:2023 is a proprietary standard published by MISRA (misra.org.uk) and must be purchased separately. This project targets MISRA C++:2023 guidelines and documents all deviations, but does not claim third-party-audited certification. NASA technical standards (NASA-STD-8719.13C, NASA-STD-8739.8A, NPR 7150.2D) are publicly available at standards.nasa.gov.

> **AI experiment note:** This codebase was produced by Claude Sonnet 4.6 (Anthropic) to evaluate whether an AI assistant can consistently follow safety-critical engineering requirements. Code review by GPT-5.4 (OpenAI). If you are using an AI assistant with this project, read `.claude/CLAUDE.md` and `CLAUDE.md` before making changes — both are normative.

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
13. [Standards Sources](#standards-sources)
14. [Project Standards Files](#project-standards-files)
14. [Claude Skills](#claude-skills)
15. [Release History](#release-history)
16. [Code Statistics](#code-statistics)

---

## Requirements Overview

### 1. Message Envelope
Every message is carried in a standard `MessageEnvelope` containing:
- **Message type** — DATA, ACK, NAK, HEARTBEAT, or HELLO
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
- Fixed 52-byte header; payload follows immediately
- Protocol version byte (byte 3) and 2-byte frame magic `0x4D45` ('ME') embedded in every frame — mismatched version or magic rejected before any field is read (`src/core/ProtocolVersion.hpp`)
- Validated on deserialization: protocol version, frame magic, payload length bounds

### 4. Transport Abstraction
A single `TransportInterface` API (`init`, `send_message`, `receive_message`, `close`) hides all transport differences. Five implementations are provided:

| Backend | Description |
|---|---|
| **TcpBackend** | Connection-oriented; length-prefixed framing; multi-client server support; client sends HELLO on connect; server maintains NodeId→slot routing table; source_id validated on every receive frame (REQ-6.1.8–6.1.11) |
| **UdpBackend** | Connectionless; one datagram per message; no built-in reliability; HELLO-before-data enforcement; source_id binding after first HELLO (REQ-6.2.4) |
| **TlsTcpBackend** | TLS-encrypted TCP (mbedTLS 4.0 PSA Crypto); per-client TLS context; plaintext fallback for tests; TLS session resumption (`session_resumption_enabled` in `TlsConfig`); peer hostname verification via `mbedtls_ssl_set_hostname()` when `verify_peer=true` (SEC-021 / REQ-6.4.6) |
| **DtlsUdpBackend** | DTLS-encrypted UDP; DTLS cookie anti-replay; MTU enforcement (1,400 bytes); plaintext fallback; HELLO-before-data enforcement; two-phase port-locking (SEC-027 — candidate port committed only after valid HELLO); bidirectional HELLO registration (SEC-026 — server replies to client HELLO so both sides can send DATA) |
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

`ImpairmentEngine::process_inbound()` is now active on the receive path of **all five backends**: `UdpBackend`, `DtlsUdpBackend`, `TcpBackend`, `TlsTcpBackend`, and `LocalSimHarness` (via `deliver_from_peer()`). Inbound impairments (partition drops, reordering) are applied to received messages before they are pushed to the inbound ring buffer, matching the behaviour of `process_outbound()` on the send path. `LocalSimHarness::inject()` remains a raw test bypass that skips impairment by contract.

### 6. Reliability Helpers
- **Duplicate filter** — sliding window of 128 `(source_id, message_id)` pairs; evicts the oldest on overflow
- **ACK tracker** — 32-slot fixed table; tracks pending ACKs with deadlines; sweeps timed-out entries
- **Retry manager** — per-message exponential backoff scheduler; doubles interval each attempt, capped at 60 s; cancelled on ACK

### 7. Observability
- Severity-tagged logging (`INFO`, `WARNING_LO`, `WARNING_HI`, `FATAL`) to stderr via a fixed 512-byte stack buffer
- No heap allocation in the logger
- **DeliveryEvent ring** — a bounded, overwrite-on-full ring buffer (`DeliveryEventRing`, capacity `DELIVERY_EVENT_RING_CAPACITY`) records 8 event kinds (SEND_OK, SEND_FAIL, ACK_RECEIVED, RETRY_FIRED, ACK_TIMEOUT, DUPLICATE_DROP, EXPIRY_DROP, MISROUTE_DROP). Events are pulled via `DeliveryEngine::poll_event()` — no callbacks, no heap, no virtual dispatch for delivery (REQ-7.2.5).
- **drain_events()** — bulk observability drain: `DeliveryEngine::drain_events(out_buf, buf_cap)` pulls up to `buf_cap` events in one call (FIFO order); all 8 event kinds emitted from core delivery paths.

### 8. Request/Response Helper Layer
- **RequestReplyEngine** — bounded request/response layer on top of `DeliveryEngine`. Correlation metadata travels as a 12-byte `RRHeader` prefix inside the existing `payload` field; `MessageEnvelope`, `Serializer`, and `PROTO_VERSION` are untouched. `MAX_PENDING_REQUESTS = 16`; `send_request()` uses `RELIABLE_RETRY`; `send_response()` uses `BEST_EFFORT`; `sweep_timeouts()` frees expired pending slots.

### 9. Determinism and Testability
- All components are testable without real network hardware via `LocalSimHarness`
- `ImpairmentEngine` is fully reproducible given the same seed
- External dependencies are injectable via `TransportInterface` (pointer passed at `init`)

---

## Architecture

Dependencies flow strictly downward. No lower layer may reference a higher one; cyclic dependencies are prohibited.

### Layer Overview

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  App Layer  (src/app/)                                                        │
│                                                                               │
│       Server (TCP echo)                    Client (TCP demo)                  │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │  uses DeliveryEngine + TransportInterface*
┌────────────────────────────────▼─────────────────────────────────────────────┐
│  Core Layer  (src/core/)                                                      │
│                                                                               │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │  DeliveryEngine                                                         │  │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐  │  │
│  │  │   AckTracker     │  │  RetryManager    │  │  DuplicateFilter     │  │  │
│  │  │  32-slot table   │  │  exp. backoff    │  │  128-entry window    │  │  │
│  │  │  FREE/PENDING/   │  │  INACTIVE/       │  │  (src,msg_id) pairs  │  │  │
│  │  │  ACKED states    │  │  WAITING/DUE/    │  │  FIFO eviction       │  │  │
│  │  │                  │  │  EXHAUSTED/EXPD  │  │                      │  │  │
│  │  └──────────────────┘  └──────────────────┘  └──────────────────────┘  │  │
│  │  ┌──────────────────┐                                                    │  │
│  │  │  MessageIdGen    │  monotonically incrementing uint64; never 0        │  │
│  │  └──────────────────┘                                                    │  │
│  └──────────────────────────────────┬─────────────────────────────────────┘  │
│                                     │  TransportInterface* (injected)         │
│  ┌──────────────────────────────────┴─────────────────────────────────────┐  │
│  │  Foundations                                                            │  │
│  │  Serializer · MessageEnvelope · RingBuffer (64-slot SPSC lock-free)    │  │
│  │  Timestamp  · MessageId       · Logger    · Assert / AssertState       │  │
│  │  TransportInterface (abstract) · ChannelConfig · TlsConfig · Types     │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │  implements TransportInterface
┌────────────────────────────────▼─────────────────────────────────────────────┐
│  Platform Layer  (src/platform/)                                              │
│                                                                               │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────────┐ ┌──────────────────┐   │
│  │ TcpBackend  │ │ UdpBackend  │ │ TlsTcpBackend   │ │ DtlsUdpBackend   │   │
│  │ server+client│ │ datagram   │ │ TLS over TCP    │ │ DTLS over UDP    │   │
│  │ 4-byte frame │ │ one dgram  │ │ (mbedTLS 4.0)   │ │ (mbedTLS 4.0)    │   │
│  │ 8 clients max│ │ per message│ │ cert/key/handshk│ │ cookie exchange  │   │
│  └──────┬──────┘ └──────┬──────┘ └────────┬────────┘ └────────┬─────────┘   │
│         │               │                 │                    │              │
│         └───────────────┴─────────────────┴────────────────────┘             │
│                                     │                                         │
│         each backend owns ──────────▼──────────────────────────────────────  │
│         ┌──────────────────────────────────────────────────────────────────┐  │
│         │  ImpairmentEngine                                                │  │
│         │  ┌──────────────┐  ┌───────────────────┐  ┌──────────────────┐  │  │
│         │  │  PrngEngine  │  │  Delay Buf [32]   │  │  Reorder Buf[32] │  │  │
│         │  │  xorshift64  │  │  latency / jitter │  │  reorder window  │  │  │
│         │  │  seedable    │  │  SLOT_FREE /      │  │  SLOT_FREE /     │  │  │
│         │  │              │  │  SLOT_HELD /READY │  │  SLOT_HELD/READY │  │  │
│         │  └──────────────┘  └───────────────────┘  └──────────────────┘  │  │
│         │  loss · duplication · partition (UNINIT/READY/NO_PART/PART_ACT) │  │
│         └──────────────────────────────────────────────────────────────────┘  │
│         ┌──────────────────────────────────────────────────────────────────┐  │
│         │  RingBuffer [64]  (each backend owns one inbound recv queue)     │  │
│         └──────────────────────────────────────────────────────────────────┘  │
│                                                                               │
│  LocalSimHarness (in-process, no sockets)   ImpairmentConfigLoader           │
│  SocketUtils · ISocketOps / SocketOpsImpl   IMbedtlsOps / MbedtlsOpsImpl    │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │  POSIX sockets · mbedTLS 4.0 · CLOCK_MONOTONIC
┌────────────────────────────────▼─────────────────────────────────────────────┐
│  OS / External Libraries                                                      │
│  Berkeley sockets  ·  POSIX clocks (CLOCK_MONOTONIC)  ·  mbedTLS 4.0        │
└───────────────────────────────────────────────────────────────────────────────┘
```

### Send Path (outbound)

```
App
 │  fills MessageEnvelope (type, payload, reliability, expiry, destination)
 ▼
DeliveryEngine::send()
 │  assigns message_id (MessageIdGen)  ·  stamps source_id
 │  RELIABLE_ACK/RETRY → AckTracker::track()
 │  RELIABLE_RETRY     → RetryManager::schedule()
 ▼
TransportInterface::send_message()  [virtual dispatch to concrete backend]
 ▼
TcpBackend / UdpBackend / TlsTcpBackend / DtlsUdpBackend
 │  Serializer::serialize() → wire bytes (52-byte header + payload)
 ▼
ImpairmentEngine::process_outbound()
 │  partition active?  → drop, return ERR_IO
 │  loss roll?         → drop, return ERR_IO
 │  duplication?       → queue extra copy with short time offset
 │  latency/jitter?    → queue to delay buffer with release_us
 ▼
ImpairmentEngine::collect_deliverable()  [releases delay-buffer slots]
 ▼
SocketUtils / mbedTLS  →  OS socket (TCP frame / UDP datagram / DTLS record)
```

### Receive Path (inbound)

```
OS socket  →  SocketUtils / mbedTLS
 │  TCP: socket_recv_exact (length-prefixed frame)
 │  UDP: socket_recv_from  (one datagram)
 │  DTLS: IMbedtlsOps::ssl_read via MbedtlsOpsImpl
 ▼
Serializer::deserialize()
 │  validates protocol version byte · frame magic · payload length bounds
 ▼
ImpairmentEngine::process_inbound()
 │  reorder window: hold or release
 ▼
RingBuffer::push()  [backend inbound queue]
 ▼
TransportInterface::receive_message()  [pops from RingBuffer]
 ▼
DeliveryEngine::receive()
 │  timestamp_expired()?       → return ERR_EXPIRED  (drop stale)
 │  envelope_is_control()?
 │    ACK → AckTracker::on_ack() · RetryManager::on_ack()
 │  RELIABLE_RETRY DATA?
 │    DuplicateFilter::check_and_record() → ERR_DUPLICATE (drop) or OK
 ▼
App  [processes delivered DATA envelope]
```

### Retry Pump (called each main-loop iteration)

```
DeliveryEngine::pump_retries(now_us)
 │  RetryManager::collect_due() → envelopes whose next_retry_us ≤ now_us
 │  for each due envelope → send_via_transport() → full send path above
 │  backoff doubles each attempt; slot removed at max_retries or on ACK
 ▼
DeliveryEngine::sweep_ack_timeouts(now_us)
 │  AckTracker::sweep_expired() → frees PENDING slots past their deadline
 └  logs WARNING_HI per expired slot
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
│   │   ├── Fragmentation.hpp/cpp         # Split large payloads into wire frames
│   │   ├── ReassemblyBuffer.hpp/cpp      # Reassemble fragment streams
│   │   ├── OrderingBuffer.hpp/cpp        # Sequence-number in-order delivery gate
│   │   ├── RequestReplyEngine.hpp/cpp    # Correlated request/reply over DeliveryEngine
│   │   ├── RequestReplyHeader.hpp        # 12-byte RR wire prefix (big-endian CID)
│   │   ├── DeliveryEvent.hpp             # DeliveryEventKind enum (8 event types)
│   │   ├── DeliveryEventRing.hpp         # Fixed-capacity observability event ring
│   │   ├── DeliveryStats.hpp             # Per-engine send/receive/drop counters
│   │   ├── DeliveryEngine.hpp/cpp
│   │   ├── Version.hpp                   # ME_VERSION_STRING ("2.0.0")
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
│   ├── MockTransportInterface.hpp  # Injectable TransportInterface stub for DeliveryEngine M5 tests
│   ├── test_MessageEnvelope.cpp
│   ├── test_MessageId.cpp
│   ├── test_Timestamp.cpp
│   ├── test_Serializer.cpp
│   ├── test_DuplicateFilter.cpp
│   ├── test_AckTracker.cpp
│   ├── test_RetryManager.cpp
│   ├── test_Fragmentation.cpp
│   ├── test_ReassemblyBuffer.cpp
│   ├── test_OrderingBuffer.cpp
│   ├── test_RequestReplyEngine.cpp
│   ├── test_DeliveryEngine.cpp
│   ├── test_ImpairmentEngine.cpp
│   ├── test_ImpairmentConfigLoader.cpp
│   ├── test_LocalSim.cpp
│   ├── test_AssertState.cpp
│   ├── test_SocketUtils.cpp
│   ├── test_TcpBackend.cpp
│   ├── test_UdpBackend.cpp
│   ├── test_TlsTcpBackend.cpp
│   ├── test_DtlsUdpBackend.cpp
│   ├── test_PrngEngine.cpp
│   ├── test_RingBuffer.cpp
│   └── test_stress_capacity.cpp    # Stress suite (run via make run_stress_tests)
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
| **mbedTLS** | 4.0 (PSA Crypto) | TLS and DTLS backends (`TlsTcpBackend`, `DtlsUdpBackend`); located via `pkg-config` with Homebrew fallback; the code targets the mbedTLS 4.0 PSA Crypto API |

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
build/test_MessageId
build/test_Timestamp
build/test_Serializer
build/test_DuplicateFilter
build/test_AckTracker
build/test_RetryManager
build/test_Fragmentation
build/test_ReassemblyBuffer
build/test_OrderingBuffer
build/test_RequestReplyEngine
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
build/test_PrngEngine
build/test_RingBuffer
```

---

## Stress Tests

Stress tests exercise **capacity-exhaustion and slot-recycling** paths with thousands of
consecutive fill/drain cycles.  They target a different failure class than unit tests —
slot leaks, index-arithmetic wrap errors, and counter drift that only manifest under
sustained load.

```bash
# Build only
make stress_tests

# Build and run
make run_stress_tests
```

| Test | Cycles | What it catches |
|---|---|---|
| `test_ack_tracker_fill_drain_cycles` | 10 000 × 32 slots | Slot leak on `sweep_expired`; off-by-one at slot 33 |
| `test_ack_tracker_partial_ack_then_sweep` | 1 000 × 32 slots | Double-free if ACKed slot is re-swept; ghost entries after partial drain |
| `test_retry_manager_fill_ack_drain_cycles` | 5 000 × 32 slots | Slot not freed after `on_ack`; stale inactive entries blocking `schedule` |
| `test_retry_manager_max_retry_exhaustion` | 1 000 × 32 slots × 5 retries | Slot not freed at exhaustion; retry count exceeding `MAX_RETRY_COUNT`; backoff overflow |
| `test_ring_buffer_sustained_push_pop` | 64 000 single + 1 000 fill/drain | Index-wrap arithmetic across `uint32_t` boundary; FIFO ordering; `ERR_FULL`/`ERR_EMPTY` boundaries |
| `test_dedup_filter_window_wraparound` | 100 window lengths × 128 entries | Eviction pointer wrap; false positives after eviction; false negatives within current window |

**When to run stress tests** (per `CLAUDE.md §1c`): not required on every commit. Run when
any capacity constant in `src/core/Types.hpp` changes, or when the loop-bound or
slot-management logic in `AckTracker`, `RetryManager`, `RingBuffer`, or `DuplicateFilter`
is modified.

**CI:** Stress tests run automatically via `.github/workflows/stress.yml` on a nightly
schedule (2 AM UTC), on any push or PR to `main` that touches a capacity-relevant file,
and on manual dispatch from the GitHub Actions tab.

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

> **Methodology note (2026-04-06 rebaseline):** LLVM source-based coverage now counts each branch outcome (True and False) as a separate entry, roughly doubling the raw branch count relative to prior decision-level counts. All thresholds below reflect the current LLVM output. MC/DC tests 60–64 (added 2026-04-06) closed 10 previously-missed branches in `DeliveryEngine.cpp`.

| File | Branch % | SC? | Status |
|---|---|---|---|
| `core/Serializer.cpp` | 73.79% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/DuplicateFilter.cpp` | 73.13% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/AckTracker.cpp` | 64.47% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/RetryManager.cpp` | 73.25% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/DeliveryEngine.cpp` | 71.68% | SC | Ceiling — assert `[[noreturn]]` branches + init-guard paths |
| `core/AssertState.cpp` | 50.00% | NSC-infra | Ceiling — abort() False branch untestable |
| `platform/ImpairmentEngine.cpp` | 71.88% | SC | Ceiling — assert + unreachable branches |
| `platform/ImpairmentConfigLoader.cpp` | 80.46% | SC | Ceiling — assert + unreachable branches |
| `platform/TcpBackend.cpp` | 68.97% | SC | Ceiling — assert + unreachable branches |
| `platform/UdpBackend.cpp` | 70.10% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/TlsTcpBackend.cpp` | 70.01% | SC | Ceiling — assert + mbedTLS error paths |
| `platform/DtlsUdpBackend.cpp` | 75.56% | SC | Ceiling — assert + mbedTLS/POSIX error paths |
| `platform/LocalSimHarness.cpp` | 70.49% | SC | Ceiling — assert + structurally-unreachable branches |
| `platform/MbedtlsOpsImpl.cpp` | 70.33% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/SocketUtils.cpp` | 61.44% | NSC | Ceiling — POSIX errors unreachable on loopback |
| `platform/SocketOpsImpl.cpp` | 66.67% | NSC | Line coverage sufficient |

Ceiling files are at the maximum achievable coverage: `NEVER_COMPILED_OUT_ASSERT` generates permanently-missed branch outcomes (the `[[noreturn]]` `abort()` path), and certain POSIX/mbedTLS error paths cannot be triggered in a loopback test environment. All are documented deviations, not defects. Full per-file justifications are in `docs/COVERAGE_CEILINGS.md`.

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
| `ERR_AGAIN` | More data needed — reassembly or ordering in progress; not an error |
| `ERR_IO_PARTIAL` | Partial fragmented send: ≥1 fragment already on wire; do not cancel AckTracker/RetryManager slots |

---

## Use Cases

The [`docs/use_cases/`](docs/use_cases/) directory contains detailed use case documents that trace every user-facing capability and system-internal sub-function through the live source code.

- **[HIGH_LEVEL_USE_CASES.md](docs/use_cases/HIGH_LEVEL_USE_CASES.md)** — index of all 61 use cases, grouped by high-level capability (HL-1 through HL-29), Application Workflow patterns, and System Internal sub-functions.
- **[USE_CASE_FREQUENCY.md](docs/use_cases/USE_CASE_FREQUENCY.md)** — frequency classification of all 61 use cases (hottest path → high → medium → low → system internals); use this to guide performance analysis, profiling focus, and code review prioritisation.
  - [Hottest path](docs/use_cases/USE_CASE_FREQUENCY.md#hottest-path--called-on-every-message-and-every-event-loop-tick) — 7 use cases on every send / receive / event-loop tick
  - [High frequency](docs/use_cases/USE_CASE_FREQUENCY.md#high-frequency--called-frequently-during-active-messaging) — per-message use cases when reliability modes or impairments are active

Each individual `UC_*.md` document follows a 15-section flow-of-control format covering: entry points, end-to-end control flow, call tree, branching logic, concurrency behavior, memory ownership, error handling, external interactions, state changes, and known risks.

---

## Safety & Assurance Documents

NASA-STD-8719.13C / NASA-STD-8739.8A compliance artifacts maintained in [`docs/`](docs/):

| Document | Description |
|---|---|
| [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md) | Software Safety Hazard Analysis (HAZ-001–HAZ-007), Failure Mode and Effects Analysis (FMEA) for every major component, and Safety-Critical (SC) vs Non-Safety-Critical (NSC) classification for every public function in `src/`. |
| [STATE_MACHINES.md](docs/STATE_MACHINES.md) | Formal state-transition tables and invariants for the three safety-critical state machines: `AckTracker`, `RetryManager`, and `ImpairmentEngine` (including the partition sub-state). |
| [TRACEABILITY_MATRIX.md](docs/TRACEABILITY_MATRIX.md) | Bidirectional requirements traceability matrix mapping every `[REQ-x.x]` ID from `CLAUDE.md` to the `src/` file that implements it and the `tests/` file that verifies it. |
| [STACK_ANALYSIS.md](docs/STACK_ANALYSIS.md) | Worst-case stack depth analysis across five call chains; worst-case frame depth is 10 frames, worst-case stack size is ~764 bytes (DTLS outbound send). |
| [WCET_ANALYSIS.md](docs/WCET_ANALYSIS.md) | Worst-case execution time analysis expressed as closed-form operation counts for every SC function, derived from the compile-time capacity constants in `src/core/Types.hpp`. |
| [MCDC_ANALYSIS.md](docs/MCDC_ANALYSIS.md) | MC/DC coverage analysis for the five highest-hazard SC functions: `DeliveryEngine::send`, `DeliveryEngine::receive`, `DuplicateFilter::check_and_record`, `Serializer::serialize`, `Serializer::deserialize`. |
| [INSPECTION_CHECKLIST.md](docs/INSPECTION_CHECKLIST.md) | Moderator-led formal inspection checklist (NPR 7150.2D §3 / NASA-STD-8739.8); entry/exit criteria, severity definitions, and waiver policy for all `src/` changes. |
| [DEFECT_LOG.md](docs/DEFECT_LOG.md) | Cumulative inspection defect record; every defect found during formal review of `src/` changes is logged here with disposition and sign-off. |
| [VERIFICATION_POLICY.md](docs/VERIFICATION_POLICY.md) | Verification policy (VVP-001); defines the minimum verification methods (M1–M7) required at each software classification level (Class C / B / A), architectural ceiling rules, fault injection requirements, and injectable interface obligations. |
| [SECURITY_ASSUMPTIONS.md](docs/SECURITY_ASSUMPTIONS.md) | Security assumptions the library relies on but cannot enforce: monotonic time contract (SEC-007), unique message IDs per session, constant peer node IDs, transport-layer source address validation (HELLO-before-data, source_id binding across all four backends), TLS/DTLS peer hostname verification, cryptographic material zeroing, and timing-safe comparisons. Every assumption violation is a contract breach with undefined behavior. |

### Document Dependencies

Each Safety & Assurance document depends on the following inputs. Update the listed dependencies whenever a structural change is made.

#### [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §11, §13 (NASA-STD-8719.13C, NASA-STD-8739.8A) |
| **Source files** | All `src/core/` and `src/platform/` `.hpp` files (SC/NSC classification tables) |
| **Other docs** | — (primary artifact; others depend on it) |

#### [STATE_MACHINES.md](docs/STATE_MACHINES.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §15 (NASA-STD-8719.13C §4, NASA-STD-8739.8A §6) |
| **Source files** | `AckTracker.hpp/cpp`, `RetryManager.hpp/cpp`, `ImpairmentEngine.hpp/cpp`, `Types.hpp` (capacity constants) |
| **Other docs** | [`HAZARD_ANALYSIS.md`](docs/HAZARD_ANALYSIS.md) §1 (HAZ IDs) |

#### [TRACEABILITY_MATRIX.md](docs/TRACEABILITY_MATRIX.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §11, `.claude/CLAUDE.md` §10 |
| **Source files** | All `src/` and `tests/` files (`// Implements:` / `// Verifies:` comments); REQ IDs from `CLAUDE.md` §§3–7 |
| **Other docs** | — (generated by `docs/check_traceability.sh`) |

#### [STACK_ANALYSIS.md](docs/STACK_ANALYSIS.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §13 (NASA-STD-8719.13C) |
| **Source files** | `src/app/` entry points; `DeliveryEngine`, `Serializer`, `AckTracker`, `RetryManager` (core); `TcpBackend`, `DtlsUdpBackend`, `ImpairmentEngine`, `IMbedtlsOps`, `MbedtlsOpsImpl` (platform) |
| **Other docs** | — |

#### [WCET_ANALYSIS.md](docs/WCET_ANALYSIS.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §14 (NASA-STD-8719.13C) |
| **Source files** | `src/core/Types.hpp` (capacity constants); all SC function implementation files |
| **Other docs** | [`HAZARD_ANALYSIS.md`](docs/HAZARD_ANALYSIS.md) §3 (SC function scope); [`STACK_ANALYSIS.md`](docs/STACK_ANALYSIS.md) (worst-case call chain) |

#### [MCDC_ANALYSIS.md](docs/MCDC_ANALYSIS.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §14 §2 (DO-178C, NASA-STD-8739.8A) |
| **Source files** | `DeliveryEngine.cpp`, `DuplicateFilter.cpp`, `Serializer.cpp`, `Assert.hpp`; `tests/test_DeliveryEngine.cpp`, `tests/test_DuplicateFilter.cpp`, `tests/test_Serializer.cpp` |
| **Other docs** | [`HAZARD_ANALYSIS.md`](docs/HAZARD_ANALYSIS.md) §3 (five highest-hazard SC functions); [`VERIFICATION_POLICY.md`](docs/VERIFICATION_POLICY.md) (architectural ceiling rules); `CLAUDE.md` §14 |

#### [INSPECTION_CHECKLIST.md](docs/INSPECTION_CHECKLIST.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §12 (NPR 7150.2D, NASA-STD-8739.8) |
| **Source files** | Files under review (varies per inspection) |
| **Other docs** | `.claude/CLAUDE.md` §§2–5 (Power of 10, MISRA, F-Prime, architecture — Parts C–E); `CLAUDE.md` §§6–7, §11 (error handling, security, traceability — Parts B, F, G); [`DEFECT_LOG.md`](docs/DEFECT_LOG.md) (completed checklists filed there) |

#### [DEFECT_LOG.md](docs/DEFECT_LOG.md)

| Field | Value |
|---|---|
| **Normative policy** | `CLAUDE.md` §12.3 (NPR 7150.2D) |
| **Source files** | Files reviewed in each inspection |
| **Other docs** | [`INSPECTION_CHECKLIST.md`](docs/INSPECTION_CHECKLIST.md); [`HAZARD_ANALYSIS.md`](docs/HAZARD_ANALYSIS.md) (HAZ IDs); [`VERIFICATION_POLICY.md`](docs/VERIFICATION_POLICY.md) (verification method references) |

#### [VERIFICATION_POLICY.md](docs/VERIFICATION_POLICY.md)

| Field | Value |
|---|---|
| **Normative policy** | NPR 7150.2D, NASA-STD-8739.8A, NASA-STD-8719.13C |
| **Source files** | — (process/policy document) |
| **Other docs** | [`HAZARD_ANALYSIS.md`](docs/HAZARD_ANALYSIS.md) §3 (SC/NSC definitions); `CLAUDE.md` §14 (coverage requirements) |

#### Dependency Summary

| Document | Depends on |
|---|---|
| [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md) | — (primary artifact; no doc dependencies) |
| [STATE_MACHINES.md](docs/STATE_MACHINES.md) | [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md) |
| [TRACEABILITY_MATRIX.md](docs/TRACEABILITY_MATRIX.md) | — (generated from source; no doc dependencies) |
| [STACK_ANALYSIS.md](docs/STACK_ANALYSIS.md) | — |
| [WCET_ANALYSIS.md](docs/WCET_ANALYSIS.md) | [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md), [STACK_ANALYSIS.md](docs/STACK_ANALYSIS.md) |
| [MCDC_ANALYSIS.md](docs/MCDC_ANALYSIS.md) | [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md), [VERIFICATION_POLICY.md](docs/VERIFICATION_POLICY.md) |
| [INSPECTION_CHECKLIST.md](docs/INSPECTION_CHECKLIST.md) | [DEFECT_LOG.md](docs/DEFECT_LOG.md) |
| [DEFECT_LOG.md](docs/DEFECT_LOG.md) | [INSPECTION_CHECKLIST.md](docs/INSPECTION_CHECKLIST.md), [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md), [VERIFICATION_POLICY.md](docs/VERIFICATION_POLICY.md) |
| [VERIFICATION_POLICY.md](docs/VERIFICATION_POLICY.md) | [HAZARD_ANALYSIS.md](docs/HAZARD_ANALYSIS.md) |
| [SECURITY_ASSUMPTIONS.md](docs/SECURITY_ASSUMPTIONS.md) | — (primary artifact; no doc dependencies) |

---

## Coding Standards

All production code (`src/`) is written to the following standards. Where a rule cannot be followed, a deviation is recorded at the point of use with a justification comment; significant deviations are also catalogued in `.claude/CLAUDE.md`. No rule is silently broken.

**Standards targeting note:** This project targets these standards as a development discipline. MISRA C++:2023 is a proprietary document (purchase required; not redistributed here). NASA standards are public. Neither imposes a software license on compliant code; see the [License](#license) section at the bottom of this file.

**Software classification:** NASA-STD-8719.13C / NASA-STD-8739.8A **Class C** (infrastructure / networking library). **Verification discipline voluntarily targets Class B**: all safety-critical functions require branch coverage (M4), mandatory peer inspection (M1), and static analysis (M2); MC/DC coverage (M5) is the goal for the five highest-hazard functions. See `docs/HAZARD_ANALYSIS.md` and `docs/MCDC_ANALYSIS.md`.

| Standard / Property | How this project targets it |
|---|---|
| **JPL Power of 10** | No goto, no recursion; fixed loop bounds; no dynamic allocation after init; cyclomatic complexity ≤ 10 per function (enforced by `make lint`); ≥ 2 assertions per function; minimal variable scope; all return values checked; minimal preprocessor use; ≤ 1 pointer indirection; zero compiler warnings |
| **MISRA C++:2023** | Required rules enforced; advisory rules followed; all deviations documented in-code and in `.claude/CLAUDE.md` |
| **F-Prime subset** | ISO C++17; `-fno-exceptions`; `-fno-rtti`; no templates; no explicit function pointers (vtable-backed virtual dispatch is a documented exception); no STL except `std::atomic` (see below) |
| **Error handling** | All errors returned via `Result` enum (`OK`, `ERR_TIMEOUT`, `ERR_FULL`, `ERR_IO`, …); no exceptions |
| **Assertions** | `NEVER_COMPILED_OUT_ASSERT(cond)` — always compiled in; in debug/test builds calls `abort()`; in production logs FATAL and triggers a controlled reset |
| **Layering** | App → Core → Platform → OS; no upward dependencies; no cyclic dependencies |

**`std::atomic` exceptions to the no-STL rule** (see `.claude/CLAUDE.md §3`):

| File | Exception | Justification |
|---|---|---|
| `src/core/RingBuffer.hpp` | `std::atomic<uint32_t>` | Lock-free head/tail indices; no dynamic allocation; maps directly to a hardware primitive |
| `src/core/AssertState.hpp/cpp` | `std::atomic<bool>` | `g_fatal_fired` flag must be visible across threads without a mutex |
| `src/core/Assert.hpp` | `std::memory_order_*` | Required by `std::atomic` store in the assert macro |

### Work Required to Achieve Formal Class B Classification

The project voluntarily meets Class B verification rigor (M1 + M2 + M4 + M5 for all SC functions) but is formally classified **Class C**. Formal reclassification to Class B requires completing the following items. The full gap list is in [`TODO_FOR_CLASS_B_CERT.txt`](TODO_FOR_CLASS_B_CERT.txt).

#### Already achieved (no further work needed)

| Item | Evidence |
|---|---|
| M1 — Code review / inspection | [`docs/DEFECT_LOG.md`](docs/DEFECT_LOG.md) INSP-001 (2026-03-31) |
| M2 — Static analysis (compiler + clang-tidy + cppcheck) | `make lint`; zero unresolved findings |
| M4 — Branch coverage of all SC functions | `make coverage`; 100% of reachable branches; ceilings documented in `CLAUDE.md §14` |
| M5 — Fault injection for all SC dependency-failure paths | Injectable interfaces (`IMbedtlsOps`, `ISocketOps`); fault-injection tests in `tests/` |
| MC/DC analysis (M6 goal, five highest-hazard SC functions) | [`docs/MCDC_ANALYSIS.md`](docs/MCDC_ANALYSIS.md) — all decisions demonstrated |

#### Remaining work (external tools or personnel required)

| # | Item | What is needed | Blocker |
|---|---|---|---|
| 6 | **TLA+ model checking** | Formally verify the three SC state machines (`AckTracker`, `RetryManager`, `ImpairmentEngine`) against all invariants in [`docs/STATE_MACHINES.md`](docs/STATE_MACHINES.md) using TLA+ or SPIN | TLA+ toolchain + team member with TLA+ authoring experience |
| 7 | **Frama-C WP bounds proof** | Prove absence of buffer overflow in `Serializer::serialize()` and `Serializer::deserialize()` using the Frama-C WP plugin with ACSL annotations | Frama-C installation + ACSL annotation authoring |
| 8 | **PC-lint Plus MISRA C++:2023 report** | Run PC-lint Plus with the MISRA C++:2023 ruleset over all of `src/` to produce a formal compliance report (`make pclint` is a documented TODO stub) | PC-lint Plus commercial licence ([gimpel.com](https://gimpel.com)) |
| 9 | **Independent V&V** | A second qualified engineer (not the author) must complete a structured inspection of all SC functions per [`docs/INSPECTION_CHECKLIST.md`](docs/INSPECTION_CHECKLIST.md) and record findings in [`docs/DEFECT_LOG.md`](docs/DEFECT_LOG.md) (INSP-002 onward) | Second qualified reviewer |

Items 6 and 7 are also required for Class A reclassification; Item 9 is required at both Class B and Class A (NPR 7150.2D §3.11). Item 8 closes the Tier 3 static analysis gap (see [`docs/STATIC_ANALYSIS_TOOLCHAIN.md`](docs/STATIC_ANALYSIS_TOOLCHAIN.md)).

---

## Standards Sources

[`docs/STANDARDS_SOURCES.md`](docs/STANDARDS_SOURCES.md) lists every external standard, guideline, and rule set referenced in the project's coding standards files — where each one comes from, whether it is free or proprietary, and a plain-English summary of what it is and what obligations it imposes on this codebase.

---

## Project Standards Files

Two CLAUDE.md files govern all code in this repository and divide responsibility cleanly:

| File | Purpose |
|---|---|
| `.claude/CLAUDE.md` | **Global C/C++ coding standard.** Contains all 10 JPL Power of 10 rules, MISRA C++:2023 compliance requirements, F-Prime style subset, architecture/layering rules, security posture, and NASA assurance mindset. Also cross-references `docs/VERIFICATION_POLICY.md` and `CLAUDE.md` §§11/13 for traceability and safety obligations. |
| `CLAUDE.md` | **Project-specific spec.** Contains everything tied to this repository: numbered application requirements (`[REQ-x.x]`), references to `docs/` artifacts, the per-directory rule compliance table, named static analysis toolchain, `NEVER_COMPILED_OUT_ASSERT` policy, traceability rules, formal inspection process, and safety/coverage/WCET/formal-methods obligations. |
| `docs/VERIFICATION_POLICY.md` | **Verification policy (VVP-001).** Defines the minimum verification methods (M1–M7) required at each software classification level (Class C / B / A) and the rules governing architectural ceiling arguments, fault injection, and injectable interface requirements. |

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
| **validate-safety-doc** | `/validate-safety-doc <DOC>` or `/validate-safety-doc all` | Validates one or all Safety & Assurance documents in `docs/` against the live source code. Checks that all claims, constants, function names, state machines, HAZ IDs, REQ IDs, and structural references are still accurate. Pass a document name (e.g., `/validate-safety-doc HAZARD_ANALYSIS`) to validate one document, or pass `all` to validate all nine in parallel. Reports findings by severity (CRITICAL / STALE / MISSING / MINOR) and offers to apply fixes. See below for how `all` mode works. |

#### How `/validate-safety-doc all` works

All nine documents are validated simultaneously using three dependency-ordered waves. Each document runs in its own sub-agent so validation work is fully parallel within each wave. A wave does not start until all agents in the previous wave have returned their findings — so dependent documents always receive accurate cross-check context.

**Wave 1 — independent documents (run in parallel):**

| Agent | Document | Outputs to downstream waves |
|---|---|---|
| 1 | `HAZARD_ANALYSIS.md` | Confirmed HAZ ID list; confirmed SC function list |
| 2 | `TRACEABILITY_MATRIX.md` | Confirmed REQ ID list |
| 3 | `STACK_ANALYSIS.md` | Worst-case call chain name and frame count |

**Wave 2 — depend on Wave 1 (run in parallel after Wave 1 completes):**

| Agent | Document | Dependencies used |
|---|---|---|
| 4 | `STATE_MACHINES.md` | HAZ IDs from Agent 1 |
| 5 | `WCET_ANALYSIS.md` | SC function list from Agent 1; worst-case chain from Agent 3 |
| 6 | `VERIFICATION_POLICY.md` | SC/NSC criteria from Agent 1; outputs confirmed M1–M7 labels |

**Wave 3 — depend on Wave 1 and/or Wave 2 (run in parallel after Wave 2 completes):**

| Agent | Document | Dependencies used |
|---|---|---|
| 7 | `MCDC_ANALYSIS.md` | SC function list from Agent 1; ceiling rules from Agent 6 |
| 8 | `DEFECT_LOG.md` | HAZ IDs from Agent 1; M1–M7 labels from Agent 6; reads `INSPECTION_CHECKLIST.md` directly |
| 9 | `INSPECTION_CHECKLIST.md` | Reads `DEFECT_LOG.md` directly for severity/disposition code consistency |

**Final step:** All findings are aggregated into a single report (status + finding counts per document, then a consolidated findings table sorted CRITICAL → STALE → MISSING → MINOR). If fixes are accepted, they are applied in dependency order — Wave 1 documents first — so upstream corrections are in place before downstream documents are updated.

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
    ├── find_missing_use_cases/
    │   └── SKILL.md
    └── validate-safety-doc/
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

## Release History

| Version | Highlights |
|---|---|
| **2.0.0** | Bounded fragmentation and reassembly (`Fragmentation`, `ReassemblyBuffer`); per-peer in-order delivery enforcement (`OrderingBuffer`); wire-format v2 (`WIRE_HEADER_SIZE` 44→52 bytes, `PROTO_VERSION` 1→2) |
| **1.3.0** | `RequestReplyEngine` (correlated request/reply over `DeliveryEngine`); `drain_events()` bulk observability drain |

---

## Code Statistics

| Category | Lines |
|---|---|
| `src/` (production + headers) | 19,400 |
| `tests/` | 25,039 |
| **Total** | **44,439** |

68 source files across 3 layers; 23 test suites + 1 stress suite + 2 shared mock headers (26 test files).

---

## License

This project is released under the **Apache License 2.0**.

Apache 2.0 was chosen deliberately for a safety-critical library:

- **Explicit patent grant (Section 3):** every contributor automatically grants users a royalty-free license to any patents that read on their contribution. MIT and BSD-3-Clause provide no patent protection. For teams doing IP clearance before deploying safety-critical software, this removes a significant legal uncertainty.
- **Patent retaliation clause:** a user who initiates patent litigation against contributors or other users loses their Apache 2.0 license immediately. This protects the safety-critical community using the library from patent weaponization.
- **Consistent with F-Prime lineage:** NASA/JPL released F-Prime (the framework whose style subset this project targets) under Apache 2.0. The same license signals the same engineering heritage.

**Standards note:** Compliance with MISRA C++:2023, JPL Power of 10, and NASA standards is a development process choice, not a license obligation. Neither MISRA nor NASA impose any software license on code that follows their guidelines. The MISRA standard documents themselves are proprietary and must be purchased from [misra.org.uk](https://misra.org.uk); they are not included in this repository. NASA technical standards are publicly available at [standards.nasa.gov](https://standards.nasa.gov).
