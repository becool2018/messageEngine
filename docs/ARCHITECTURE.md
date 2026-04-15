# Architecture — Project Goals and Layered Design

**Last updated:** 2026-04-15

---

## 1. Project Goals and Scope

messageEngine provides a shared network abstraction that:

- **Models realistic communication problems** — latency, jitter, packet loss, duplication, reordering, and network partitions are all first-class configurable behaviours via the `ImpairmentEngine`.
- **Offers consistent, reusable messaging utilities** — serialization, ACK tracking, retry management, duplicate suppression, expiry, fragmentation, reassembly, and ordering are provided as composable building blocks.
- **Supports multiple transports** — TCP (plaintext and TLS), UDP (plaintext and DTLS), and an in-process `LocalSimHarness` for testing, all behind a single `TransportInterface`.

All components are designed for:

- **Deterministic behaviour under test** — the `PrngEngine` is seedable; the `LocalSimHarness` requires no OS sockets; all timing is injected via `IPosixSyscalls`.
- **Strong observability** — structured logging via the injectable `Logger` / `ILogClock` / `ILogSink` pipeline; a bounded observability event ring for per-message delivery events.
- **Easy unit and integration testing without a real network** — every external dependency (sockets, time, mbedTLS, POSIX syscalls) is injectable via abstract interfaces; the full send/receive/retry/dedup pipeline runs in-process.

---

## 2. Layered Architecture

messageEngine is conceptually divided into four layers. Higher layers depend on lower ones; no upward or cyclic dependencies are permitted.

    +------------------------------------------+
    |  src/app  -- Application orchestration    |
    |  (DeliveryEngine, RequestReplyEngine)      |
    +------------------------------------------+
    |  src/core -- Domain logic                  |
    |  (Serializer, AckTracker, RetryManager,    |
    |   DuplicateFilter, OrderingBuffer,          |
    |   ReassemblyBuffer, Fragmentation,          |
    |   RingBuffer, MessageIdGen, Logger)         |
    +------------------------------------------+
    |  src/platform -- OS / platform adapters    |
    |  (TcpBackend, UdpBackend,                  |
    |   TlsTcpBackend, DtlsUdpBackend,           |
    |   ImpairmentEngine, LocalSimHarness,       |
    |   SocketUtils, MbedtlsOpsImpl,             |
    |   PosixLogClock, PosixLogSink)              |
    +------------------------------------------+
    |  OS / network / mbedTLS                    |
    +------------------------------------------+

### Layer Descriptions

**Message layer (src/app + src/core — application-facing)**
Defines the common `MessageEnvelope` and delivery semantics. Provides serialization/deserialization and all reliability helpers. Exposes a transport-agnostic API to the embedding application. `DeliveryEngine` is the primary entry point.

**Transport abstraction layer (src/core — TransportInterface)**
Presents a consistent `init` / `send_message` / `receive_message` / `flush` interface to `DeliveryEngine`. Hides TCP/UDP differences. Knows about priorities, reliability modes, and channels, but not application semantics.

**Impairment engine (src/platform — ImpairmentEngine)**
Sits logically between the transport abstraction and the underlying I/O backends. Intercepts outbound messages before they reach the socket and inbound messages after they arrive. Applies configured fault conditions (latency, jitter, loss, duplication, reordering, partitions). Fully injectable and deterministic.

**Platform/network backend (src/platform — socket backends)**
Encapsulates raw socket operations (TCP, UDP) and optional TLS/DTLS. Handles connection management, framing, basic error mapping, and OS specifics. Contains no application policy; only transport mechanics.

---

## 3. Directory Rules

### src/core
- Core domain logic.
- Depends **only** on abstract interfaces declared in `src/platform` (e.g., `TransportInterface`, `ISocketOps`, `IPosixSyscalls`).
- No direct POSIX, socket, or thread API calls.
- Must not include or call concrete platform implementations directly.

### src/platform
- OS and platform adapters (sockets, timers, threading, filesystem).
- No application-specific policy; only mechanics and thin wrappers.
- Implements the abstract interfaces declared in `src/core`.

### src/app
- Application orchestration and high-level behaviour.
- Uses `src/core` and `src/platform` through their public interfaces.

### tests/
- Unit and integration tests.
- See `CLAUDE.md §1b` for the exact list of permitted relaxations and non-negotiable rules.

### docs/
- Requirements, design docs, and other project documentation.

---

## 4. Layering Rules (Normative)

The following rules are enforced by `make lint` (Clang-Tidy include-hierarchy checks):

1. **Higher layers depend on lower ones only:** `app -> core -> platform -> OS`. Never the reverse.
2. **No bypassing abstractions:** `src/app` and `src/core` must not call raw OS APIs or socket functions directly; only through `src/platform` interfaces.
3. **No cyclic dependencies** between modules or directories.

Before making significant changes, briefly summarize how the planned change fits the layering rules and any relevant module requirements in `CLAUDE.md §§3–7`.
