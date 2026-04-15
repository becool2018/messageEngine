# messageEngine

> This codebase is a learning and research exercise largely produced with AI assistance (Claude Code).  APIs, behavior, and documentation may change without notice.

![CI](https://github.com/becool2018/messageEngine/actions/workflows/CI.yml/badge.svg)
![Stress Tests](https://github.com/becool2018/messageEngine/actions/workflows/stress.yml/badge.svg)

A C++ networking library for building and testing systems that must survive unreliable networks. It provides:

- **Three delivery modes** — best-effort, reliable-with-ACK, and reliable-with-retry-and-dedup — implemented at the application layer, transport-independent
- **Five transport backends** — TCP, UDP, TLS/TCP, DTLS/UDP, and an in-process simulation harness (`LocalSimHarness`) that needs no OS sockets
- **A fault injection engine** — inject latency, jitter, packet loss, duplication, reordering, and link partitions, all driven by a seedable PRNG for reproducible test runs
- **Bounded, fixed-allocation design** — no heap allocation on critical paths after init; all capacities are compile-time constants in `src/core/Types.hpp`
- **Observable by default** — severity-tagged logging ([guide](docs/LOGGING.md)), per-engine delivery counters, and an 8-kind event ring (`poll_event` / `drain_events`) for post-hoc analysis without callbacks or heap

**Quick capacity reference** ([full details →](docs/CAPACITY_REFERENCE.md))**:**

| Resource | Limit |
|---|---|
| Max payload | 4 096 bytes |
| Dedup window | 128 `(source, msg_id)` pairs — sliding window that detects and drops duplicate retransmissions |
| ACK tracker | 32 pending slots |
| Retry manager | 32 pending slots |
| Inbound ring | 64 messages per backend |
| Concurrent clients (TCP/TLS) | 8 (configurable via `MAX_TCP_CONNECTIONS` in `Types.hpp`) |
| Worst-case stack depth | ~130 KB / 11 frames (flush path, Chain 3); non-flush: ~764 bytes / 9 frames |

**Written to:** JPL Power of 10 · MISRA C++:2023 · F-Prime style subset · NASA Class C (voluntary Class B test rigor). No exceptions, no templates, no RTTI. STL is excluded from production code with one deliberate exception: `std::atomic<T>` for integral types is permitted and used for shared state — it has no dynamic allocation, maps directly to hardware primitives, and is what MISRA C++:2023 endorses for lock-free concurrency. All other STL containers, algorithms, and headers are absent from `src/`.

> **Standards note:** MISRA C++:2023 is a proprietary standard published by MISRA (misra.org.uk) and must be purchased separately. This project targets MISRA C++:2023 guidelines and documents all deviations, but does not claim third-party-audited certification. NASA technical standards (NASA-STD-8719.13C, NASA-STD-8739.8A, NPR 7150.2D) are publicly available at standards.nasa.gov.

> **AI experiment note:** This codebase was produced by Claude Sonnet 4.6 (Anthropic) to evaluate whether an AI assistant can consistently follow safety-critical engineering requirements. Code review by GPT-5.4 (OpenAI). If you are using an AI assistant with this project, read `.claude/CLAUDE.md` and `CLAUDE.md` before making changes — both are normative.

---

## Table of Contents

1. [Getting Started for New Contributors](#getting-started-for-new-contributors)
2. [Requirements Overview](#requirements-overview)
3. [Architecture](#architecture)
4. [Directory Structure](#directory-structure)
5. [Building](#building)
6. [Running the Tests](#running-the-tests)
7. [Coverage Analysis](#coverage-analysis)
8. [Static Analysis](#static-analysis)
9. [Running the Demo (Server / Client)](#running-the-demo-server--client) — see also [Demo Walkthrough](docs/DEMO_WALKTHROUGH.md)
10. [Logging](docs/LOGGING.md) — format, architecture, severity levels, extending, testing
11. [Documentation Maintenance](docs/DOC_MAINTENANCE.md) — what to update and when for every type of code change
12. [Using the Library](#using-the-library)
13. [Use Cases](#use-cases)
14. [Where this API could be used; Possible Applications](docs/POSSIBLE_APPLICATIONS.md)
15. [Safety & Assurance Documents](#safety--assurance-documents)
16. [Design Patterns](#design-patterns)
17. [Coding Standards](#coding-standards)
18. [Standards Sources and Conflicts](#standards-sources-and-conflicts)
19. [Project Standards Files](#project-standards-files)
20. [Claude Skills](#claude-skills)
21. [Release History](#release-history)
22. [Code Statistics](#code-statistics)
23. [Security Check](SNYK.IO.SECURITY_CHECK.md)

---

## Getting Started for New Contributors

If you are new to this codebase, the fastest path to understanding it is:

**Step 1 — Orient yourself (15 minutes)**

```
README.md                     ← you are here; read the Architecture and Directory Structure sections
docs/DEMO_WALKTHROUGH.md      ← line-by-line walkthrough of a real client/server exchange
docs/use_cases/HIGH_LEVEL_USE_CASES.md  ← all 78 capabilities in one index (HL-1 through HL-37)
```

**Step 2 — Build and run (5 minutes)**

```bash
brew install gcc make llvm cppcheck pkg-config mbedtls   # macOS
make all && make run_tests                                # build + verify all tests pass
build/server &; build/client                              # watch a real exchange
```

**Step 3 — Write your first test with LocalSimHarness (no sockets needed)**

The `LocalSimHarness` lets you exercise the full send/receive/retry/dedup pipeline entirely in-process. See "Testing with LocalSimHarness" in the [Using the Library](#using-the-library) section below, or look at any test in `tests/test_LocalSim.cpp` or `tests/test_DeliveryEngine.cpp` for working examples.

**Mental model: four layers, one direction**

```
Your app code
    ↓  DeliveryEngine (reliability, dedup, expiry, ACK, retry, ordering, fragmentation)
    ↓  TransportInterface (abstract: send/receive/init/close)
    ↓  Backend (TcpBackend / TlsTcpBackend / UdpBackend / DtlsUdpBackend / LocalSimHarness)
    ↓  ImpairmentEngine (optional: loss, jitter, latency, duplication, reordering, partitions)
    ↓  OS sockets (or in-process ring buffer for LocalSimHarness)
```

Dependencies flow downward only. `src/core/` never touches sockets; `src/platform/` never touches application logic.

**Key files for a first reading**

| File | What it teaches |
|---|---|
| `src/core/Types.hpp` | Every capacity constant and enum in the system |
| `src/core/MessageEnvelope.hpp` | The one data structure everything passes through |
| `src/core/DeliveryEngine.hpp` | The main API your application calls |
| `src/core/TransportInterface.hpp` | The abstract contract all backends implement |
| `tests/test_DeliveryEngine.cpp` | The most comprehensive integration-style test suite |

**Coding standards check (before your first change)**

```bash
/read-standards         # inside Claude Code — internalizes CLAUDE.md + .claude/CLAUDE.md
make lint               # must pass with zero warnings before any commit
make run_tests          # must pass with zero failures before any commit
```

Every public function in `src/` must have a `// Safety-critical (SC): HAZ-NNN` annotation (if SC) and a `// Implements: REQ-x.x` tag. See `CLAUDE.md §11–§13` for the full rules. When in doubt, read the existing code — the pattern is consistent throughout.

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
- Severity-tagged logging (`INFO`, `WARNING_LO`, `WARNING_HI`, `FATAL`) to stderr via a fixed 512-byte stack buffer — **[full logging guide →](docs/LOGGING.md)**
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
│   │   ├── TlsSessionStore.hpp/cpp         # Caller-owned TLS session persistence (RFC 5077)
│   │   ├── DtlsUdpBackend.hpp/cpp          # DTLS over UDP (mbedTLS 4.0 PSA)
│   │   ├── IPosixSyscalls.hpp              # Injectable POSIX syscall interface (fault injection)
│   │   ├── PosixSyscallsImpl.hpp/cpp       # Production POSIX syscall singleton
│   │   └── LocalSimHarness.hpp/cpp
│   └── app/              # Demo programs
│       ├── Server.cpp
│       ├── Client.cpp
│       ├── TlsTcpDemo.cpp                  # TLS client/server demo
│       └── DtlsUdpDemo.cpp                 # DTLS client/server demo
├── tests/                # Unit tests (one binary per module)
│   ├── FakeLogClock.hpp            # Deterministic ILogClock stub (fixed mono_us / wall_us)
│   ├── FaultPosixLogClock.hpp      # ILogClock stub that can inject clock failures
│   ├── FaultPosixLogSink.hpp       # ILogSink stub that can inject write failures
│   ├── MockSocketOps.hpp           # Injectable ISocketOps stub for all four transport backend M5 tests
│   ├── MockTransportInterface.hpp  # Injectable TransportInterface stub for DeliveryEngine M5 tests
│   ├── RingLogSink.hpp             # Fixed-capacity ring-buffer log sink for asserting log output in tests
│   ├── TestPortAllocator.hpp       # Port allocation helper for concurrent backend tests
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
│   ├── test_Logger.cpp
│   ├── test_PrngEngine.cpp
│   ├── test_RingBuffer.cpp
│   ├── test_stress_capacity.cpp    # Capacity-limit stress tests (slot exhaustion, ring overflow)
│   ├── test_stress_e2e.cpp         # End-to-end throughput and reliability stress tests
│   ├── test_stress_ordering.cpp    # Ordering gate stress tests (reorder, hold, release)
│   ├── test_stress_reconnect.cpp   # Reconnect / ordering-reset stress tests
│   ├── test_stress_ringbuffer.cpp  # RingBuffer concurrent-access stress tests
│   └── test_stress_tcp_fanin.cpp   # TCP fan-in stress tests (many clients → one server)
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
| **mbedTLS** | 4.0 (PSA Crypto) | **Optional** (required only when `TLS=1`, which is the default). TLS and DTLS backends (`TlsTcpBackend`, `DtlsUdpBackend`); located via `pkg-config` with Homebrew fallback. Build without it using `make TLS=0` — see [Building without TLS/DTLS](#building-without-tlsdtls). |

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

### Building without TLS/DTLS

If mbedTLS is not available in your environment, or you only need TCP/UDP without encryption, pass `TLS=0` to any Make target:

```bash
# Install only the base tools (no mbedtls / libmbedtls-dev required)
# macOS:  brew install gcc make llvm cppcheck pkg-config
# Linux:  sudo apt install build-essential clang clang-tidy clang-tools cppcheck llvm

make all TLS=0           # server, client, all non-TLS tests
make run_tests TLS=0     # runs 22 tests; TlsTcpBackend and DtlsUdpBackend skipped
make lint TLS=0          # lints only non-TLS source files
make cppcheck TLS=0      # checks only non-TLS source files
```

When `TLS=0`:
- `TlsTcpBackend`, `TlsSessionStore`, `DtlsUdpBackend`, and `MbedtlsOpsImpl` are excluded from compilation.
- `-DMESSAGEENGINE_NO_TLS` is injected so any conditional code in headers can guard TLS-only APIs.
- `tls_demo`, `dtls_demo`, and the `demos` target print an error and exit if invoked.
- All install and package targets omit TLS headers (`TlsConfig.hpp`, `TlsTcpBackend.hpp`, `TlsSessionStore.hpp`, `DtlsUdpBackend.hpp`, `IMbedtlsOps.hpp`).

The default (`TLS=1`) requires mbedTLS and builds all five transport backends.

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
build/test_Logger
build/test_PrngEngine
build/test_RingBuffer
```

---

## Stress Tests

Six stress test binaries cover capacity-exhaustion, sustained throughput, ordering under
reorder, reconnect recovery, RingBuffer concurrent access, and TCP fan-in.  They target a
different failure class than unit tests — slot leaks, index-arithmetic wrap errors, and
counter drift that only manifest under sustained load.

```bash
# Build the four main stress binaries (capacity, e2e, ordering, reconnect)
make stress_tests

# Build and run the three standard stress suites (capacity, e2e, ordering; ~60 s each)
make run_stress_tests

# Run the additional suites individually
make run_stress_reconnect   # peer reconnect / ordering-reset under load
make run_stress_ringbuffer  # RingBuffer concurrent-access storm
make run_stress_tcp_fanin   # TCP multi-client fan-in (many clients → one server)
```

**`test_stress_capacity` sub-tests** (each a function inside the capacity binary):

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

> **Methodology note:** LLVM source-based coverage counts each branch outcome (True and False) as a separate entry. All thresholds reflect LLVM output. Numbers are kept current in `docs/COVERAGE_CEILINGS.md` (single source of truth); the table below is a snapshot. All files are at their architectural maxima — see `docs/COVERAGE_CEILINGS.md` for per-file justifications.

| File | Branch % | SC? | Status |
|---|---|---|---|
| `core/Serializer.cpp` | 73.76% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/DuplicateFilter.cpp` | 73.13% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/AckTracker.cpp` | 76.97% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/RetryManager.cpp` | 77.07% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/DeliveryEngine.cpp` | ~76.2% | SC | Ceiling — assert `[[noreturn]]` branches + init-guard paths |
| `core/OrderingBuffer.cpp` | 79.33% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/Fragmentation.cpp` | 78.12% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/RequestReplyEngine.cpp` | 78.10% | SC | Ceiling — assert `[[noreturn]]` branches + payload-guard dead code |
| `core/AssertState.cpp` | 50.00% | NSC-infra | Ceiling — abort() False branch untestable |
| `platform/ImpairmentEngine.cpp` | 74.22% | SC | Ceiling — assert + unreachable branches |
| `platform/ImpairmentConfigLoader.cpp` | 83.91% | SC | Ceiling — assert + unreachable branches |
| `platform/TcpBackend.cpp` | 75.96% | SC | Ceiling — assert `[[noreturn]]` branches + hard POSIX error paths |
| `platform/UdpBackend.cpp` | 74.23% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/TlsTcpBackend.cpp` | 78.51% | SC | Ceiling — assert + hard mbedTLS/POSIX error paths |
| `platform/TlsSessionStore.cpp` | 66.67% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/DtlsUdpBackend.cpp` | 77.21% | SC | Ceiling — assert + mbedTLS/POSIX error paths |
| `platform/LocalSimHarness.cpp` | 70.49% | SC | Ceiling — assert + structurally-unreachable branches |
| `platform/MbedtlsOpsImpl.cpp` | 69.33% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/SocketUtils.cpp` | 75.82% | NSC | Ceiling — POSIX errors unreachable on loopback |
| `platform/PosixSyscallsImpl.cpp` | 70.37% | NSC | Line coverage sufficient |
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

For a line-by-line explanation of every log message — including HELLO registration, ACK tracking, retry pump, and clean shutdown — see **[`docs/DEMO_WALKTHROUGH.md`](docs/DEMO_WALKTHROUGH.md)**.

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

### 8. Large message fragmentation

Payloads larger than `FRAG_MAX_PAYLOAD_BYTES` (1,024 bytes, defined in `src/core/Types.hpp`) are automatically fragmented by `DeliveryEngine::send()` and reassembled on the receive side by `ReassemblyBuffer::ingest()`. Fragmentation is transparent to both sides — the caller always works with a single `MessageEnvelope`, up to `MSG_MAX_PAYLOAD_BYTES` (4,096 bytes).

```cpp
// Any payload > 1024 bytes is automatically split into ≤4 fragments.
// The receive side reassembles them and delivers a single envelope.
env.payload_length = 3000U;   // 3 fragments on wire; caller sees nothing special
Result res = engine.send(env, now_us);
// res == OK    — all fragments transmitted
// res == ERR_IO_PARTIAL — ≥1 fragment sent but not all; AckTracker/RetryManager
//                         slots are still active; do NOT cancel them manually
```

Reassembly has a bounded slot pool (`REASSEMBLY_SLOT_COUNT` slots, each keyed by `(source_id, msg_id)`). Slots that do not complete within `recv_timeout_ms` are reclaimed automatically by `sweep_ack_timeouts()` — no manual cleanup is needed (REQ-3.2.9).

---

### 9. Ordered delivery

Set `sequence_num > 0` on each outbound `DATA` envelope and configure the channel with `OrderingMode::ORDERED` to enable per-peer in-order delivery enforcement. The `OrderingBuffer` holds out-of-order envelopes until all earlier sequence numbers from the same peer have been delivered.

```cpp
// Channel config:
cfg.channels[0].ordering = OrderingMode::ORDERED;

// On each send, assign a monotonically increasing sequence number:
env.sequence_num = ++my_seq;     // 1, 2, 3, … (0 means unordered/control)

// On receive, engine.receive() returns envelopes in sequence order.
// ERR_AGAIN means a held envelope is waiting for a gap to fill — not an error.
```

Out-of-order envelopes are held in a bounded pool (`ORDERING_HOLD_COUNT` = 8 slots per peer). When a peer reconnects (new session), call `engine.drain_hello_reconnects()` or simply continue calling `engine.receive()` — the ordering state is reset automatically when the backend detects a reconnect HELLO (REQ-3.3.6).

---

### 10. TLS session resumption

`TlsTcpBackend` supports abbreviated TLS reconnects (RFC 5077 session tickets) via `TlsSessionStore`. Inject one before `init()` to persist session material across `close()` / `init()` cycles.

```cpp
#include "platform/TlsSessionStore.hpp"
#include "platform/TlsTcpBackend.hpp"

// Create a store that outlives all backends that reference it.
TlsSessionStore store;

TlsTcpBackend client;
client.set_session_store(&store);    // must be called BEFORE init()
cfg.tls.tls_enabled                  = true;
cfg.tls.session_resumption_enabled   = true;
(void)client.init(cfg);              // full handshake; session saved in store
client.close();

// Second connection: same store, session_valid == true → abbreviated handshake.
TlsTcpBackend client2;
client2.set_session_store(&store);
(void)client2.init(cfg);             // session resumption (no full cert exchange)
client2.close();

store.zeroize();                      // secure wipe; zeros key material (HAZ-017)
```

`TlsSessionStore` is non-copyable, stack-safe, and owns no heap. The `zeroize()` call uses `mbedtls_platform_zeroize()` (not `memset`) to prevent compiler-elision of the wipe.

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

- **[HIGH_LEVEL_USE_CASES.md](docs/use_cases/HIGH_LEVEL_USE_CASES.md)** — index of all 76 use cases, grouped by high-level capability (HL-1 through HL-35), Application Workflow patterns, and System Internal sub-functions.
- **[USE_CASE_FREQUENCY.md](docs/use_cases/USE_CASE_FREQUENCY.md)** — frequency classification of all 76 use cases (hottest path → high → medium → low → system internals); use this to guide performance analysis, profiling focus, and code review prioritisation.
  - [Hottest path](docs/use_cases/USE_CASE_FREQUENCY.md#hottest-path--called-on-every-message-and-every-event-loop-tick) — 7 use cases on every send / receive / event-loop tick
  - [High frequency](docs/use_cases/USE_CASE_FREQUENCY.md#high-frequency--called-frequently-during-active-messaging) — per-message use cases when reliability modes or impairments are active

Each individual `UC_*.md` document follows a 15-section flow-of-control format covering: entry points, end-to-end control flow, call tree, branching logic, concurrency behavior, memory ownership, error handling, external interactions, state changes, and known risks.

---

## Safety & Assurance Documents

Ten NASA-STD-8719.13C / NASA-STD-8739.8A compliance artifacts are maintained in [`docs/`](docs/), covering hazard analysis (HAZ-001–HAZ-025), FMEA, SC/NSC function classification, formal state machines, requirements traceability, stack and WCET analysis, MC/DC coverage, inspection records, verification policy, and security assumptions.

Full document index, descriptions, and dependency graph: **[`docs/SAFETY_ASSURANCE.md`](docs/SAFETY_ASSURANCE.md)**

---

## Design Patterns

[`docs/DESIGN_PATTERNS.md`](docs/DESIGN_PATTERNS.md) documents every design pattern used in the codebase — where each pattern appears (file, class, method), and why it was chosen given the project's Power of 10 / MISRA C++:2023 / F-Prime constraints.

Patterns in use: Strategy, Adapter/Bridge, Facade, State Machine, Observer/Event Ring, Chain of Responsibility, Non-Virtual Interface (NVI), Dependency Injection, Singleton (safety variant), Slot Pool, Value Object, Serializer, Ring Buffer (SPSC lock-free), RAII Lifecycle.

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
| M5 — Fault injection for all SC dependency-failure paths | Injectable interfaces (`IMbedtlsOps`, `ISocketOps`); `MockSocketOps` and `DtlsMockOps` test doubles; fault-injection tests across all four transport backends covering bind, connect, send, receive, and TLS/DTLS credential-path failures |
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

## Standards Sources and Conflicts

[`docs/STANDARDS_AND_CONFLICTS.md`](docs/STANDARDS_AND_CONFLICTS.md) lists every external standard, guideline, and rule set referenced in the project's coding standards files — where each one comes from, whether it is free or proprietary, and a plain-English summary of what it is and what obligations it imposes on this codebase. It also documents all known contradictions and tensions between the combined standard set, with resolution status for each.

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
| **2.1.0** | Peer reconnect ordering state reset (REQ-3.3.6 / HAZ-016): `OrderingBuffer::reset_peer()`, `DeliveryEngine::reset_peer_ordering()`, `DeliveryEngine::drain_hello_reconnects()`, `TransportInterface::pop_hello_peer()` — prevents ordering gate permanent stall when a peer reconnects with a new session (seq starting at 1) |
| **2.0.0** | Bounded fragmentation and reassembly (`Fragmentation`, `ReassemblyBuffer`); per-peer in-order delivery enforcement (`OrderingBuffer`); wire-format v2 (`WIRE_HEADER_SIZE` 44→52 bytes, `PROTO_VERSION` 1→2) |
| **1.3.0** | `RequestReplyEngine` (correlated request/reply over `DeliveryEngine`); `drain_events()` bulk observability drain |

---

## Code Statistics

| Category | Lines |
|---|---|
| `src/` (production + headers) | 23,009 |
| `tests/` | 37,993 |
| **Total** | **61,002** |

83 source files across 3 layers; 24 unit test suites + 6 stress test suites + 7 shared mock/helper headers (37 test files).

---

## License

This project is released under the **Apache License 2.0**.

Apache 2.0 was chosen deliberately for a safety-critical library:

- **Explicit patent grant (Section 3):** every contributor automatically grants users a royalty-free license to any patents that read on their contribution. MIT and BSD-3-Clause provide no patent protection. For teams doing IP clearance before deploying safety-critical software, this removes a significant legal uncertainty.
- **Patent retaliation clause:** a user who initiates patent litigation against contributors or other users loses their Apache 2.0 license immediately. This protects the safety-critical community using the library from patent weaponization.
- **Consistent with F-Prime lineage:** NASA/JPL released F-Prime (the framework whose style subset this project targets) under Apache 2.0. The same license signals the same engineering heritage.

**Standards note:** Compliance with MISRA C++:2023, JPL Power of 10, and NASA standards is a development process choice, not a license obligation. Neither MISRA nor NASA impose any software license on code that follows their guidelines. The MISRA standard documents themselves are proprietary and must be purchased from [misra.org.uk](https://misra.org.uk); they are not included in this repository. NASA technical standards are publicly available at [standards.nasa.gov](https://standards.nasa.gov).
