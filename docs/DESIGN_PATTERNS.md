# Design Patterns in messageEngine

**Project:** messageEngine
**Purpose:** Documents every design pattern used in the codebase — where each pattern appears, which files implement it, and why it was chosen given the project's Power of 10 / MISRA C++:2023 / F-Prime constraints.

---

## Table of Contents

1. [Structural Patterns](#1-structural-patterns)
   - [Strategy](#11-strategy)
   - [Adapter / Bridge](#12-adapter--bridge)
   - [Facade](#13-facade)
2. [Behavioral Patterns](#2-behavioral-patterns)
   - [State Machine](#21-state-machine)
   - [Observer / Event Ring](#22-observer--event-ring)
   - [Chain of Responsibility](#23-chain-of-responsibility)
   - [Non-Virtual Interface (NVI)](#24-non-virtual-interface-nvi)
3. [Creational Patterns](#3-creational-patterns)
   - [Dependency Injection](#31-dependency-injection)
   - [Singleton (safety variant)](#32-singleton-safety-variant)
   - [Slot Pool](#33-slot-pool)
4. [Data and Infrastructure Patterns](#4-data-and-infrastructure-patterns)
   - [Value Object](#41-value-object)
   - [Serializer](#42-serializer)
   - [Ring Buffer (SPSC lock-free)](#43-ring-buffer-spsc-lock-free)
   - [RAII Lifecycle](#44-raii-lifecycle)
5. [Patterns Deliberately Absent](#5-patterns-deliberately-absent)

---

## 1. Structural Patterns

### 1.1 Strategy

**What it is:** Define a family of interchangeable algorithms (or implementations) behind a common interface, so the context can switch between them without knowing which one is in use.

**Where used:**

| Interface | Concrete implementations |
|---|---|
| `src/core/TransportInterface.hpp` | `src/platform/TcpBackend.hpp` |
| `TransportInterface` (abstract) | `src/platform/TlsTcpBackend.hpp` |
| | `src/platform/UdpBackend.hpp` |
| | `src/platform/DtlsUdpBackend.hpp` |
| | `src/platform/LocalSimHarness.hpp` |

`DeliveryEngine` holds a `TransportInterface*` pointer. It calls `send_message()`, `receive_message()`, and `register_local_id()` through the interface and never knows which backend is active.

**Why:** The project must support TCP, TLS/TCP, UDP, DTLS/UDP, and a no-socket local simulation harness interchangeably, with no code changes to the delivery layer. Strategy is the cleanest way to achieve this while staying within MISRA C++:2023's rules on virtual dispatch (vtable-backed virtual functions are the documented exception to Power of 10 Rule 9).

---

### 1.2 Adapter / Bridge

**What it is:** Wrap an incompatible interface (POSIX sockets, mbedTLS) behind an abstract interface that higher-level code can depend on — and that tests can replace with a mock.

**Where used:**

| Abstract interface | Production implementation | Purpose |
|---|---|---|
| `src/platform/ISocketOps.hpp` | `src/platform/SocketOpsImpl.hpp` | Wraps `socket()`, `connect()`, `send()`, `recv()`, `poll()`, etc. |
| `src/platform/IMbedtlsOps.hpp` | `src/platform/MbedtlsOpsImpl.hpp` | Wraps mbedTLS handshake, read, write, and context management calls |

Test files inject mocks through these interfaces via the backend constructors (see §3.1).

**Why:** Power of 10 Rule 3 bans dynamic allocation after init; the test framework needs fault injection (socket errors, TLS failures) without monkey-patching or link-time substitution. Injectable interfaces are the only MISRA-compliant way to achieve testability of the platform layer without a real network.

---

### 1.3 Facade

**What it is:** Provide a single, simplified entry point that hides the complexity of a subsystem composed of many collaborating objects.

**Where used:**

`src/core/DeliveryEngine.hpp` — class `DeliveryEngine`

`DeliveryEngine` is the only class application code touches. Internally it owns and coordinates:

| Member | Type | Role |
|---|---|---|
| `m_ack_tracker` | `AckTracker` | Tracks outstanding ACKs |
| `m_retry_manager` | `RetryManager` | Schedules and fires retries |
| `m_dedup` | `DuplicateFilter` | Suppresses duplicate messages |
| `m_reassembly` | `ReassemblyBuffer` | Reassembles fragmented messages |
| `m_ordering` | `OrderingBuffer` | Enforces ordered delivery |
| `m_events` | `DeliveryEventRing` | Records per-message observability events |
| `m_transport` | `TransportInterface*` | The active backend (Strategy) |

The public API exposed to callers is just: `init()`, `send()`, `receive()`, `pump_retries()`, `sweep_ack_timeouts()`, `drain_events()`.

**Why:** Application code (Server.cpp, Client.cpp, TlsTcpDemo.cpp, DtlsUdpDemo.cpp) must not be exposed to the internal machinery of reliability, deduplication, and fragmentation. Facade keeps `src/app` simple and ensures the layering rule (App → Core → Platform) is enforced at the API level.

---

## 2. Behavioral Patterns

### 2.1 State Machine

**What it is:** Model an object's lifecycle as a set of named states and explicit, guarded transitions between them.

**Where used:**

#### AckTracker — `src/core/AckTracker.hpp`

```
FREE → PENDING → ACKED → FREE
```

Tracked by `enum class EntryState : uint8_t { FREE, PENDING, ACKED }` on each slot.
Transitions driven by `track()`, `on_ack()`, `cancel()`, and `sweep_expired()`.
Documented formally in `docs/STATE_MACHINES.md §1`.

#### RetryManager — `src/core/RetryManager.hpp`

```
INACTIVE → ACTIVE → INACTIVE
```

Each `RetryEntry` has an `active` bool. Transitions driven by `schedule()`, `on_ack()`, `cancel()`, and `collect_due()`.
Documented formally in `docs/STATE_MACHINES.md §2`.

#### ReassemblyBuffer — `src/core/ReassemblyBuffer.hpp`

```
INACTIVE → COLLECTING → COMPLETE
                       → INACTIVE  (timeout sweep)
```

Each `ReassemblySlot` has an `active` bool; completion is detected when all expected fragments are received. Stale slots reclaimed by `sweep_expired()`.

#### ImpairmentEngine — `src/platform/ImpairmentEngine.hpp`

```
PARTITION_INACTIVE → PARTITION_ACTIVE → PARTITION_INACTIVE
```

Tracked by `m_partition_active` bool with timestamps `m_partition_start_us` and `m_next_partition_event_us`. Transition logic in `is_partition_active(uint64_t now_us)`.
Documented formally in `docs/STATE_MACHINES.md §3`.

**Why:** State machines make lifecycle logic explicit, provably finite, and statically analysable — exactly what NASA-STD-8719.13C and Power of 10 Rule 1 (no goto, no recursion) require. Named enum states eliminate implicit integer comparisons and make FMEA analysis tractable (see `docs/HAZARD_ANALYSIS.md §2`).

---

### 2.2 Observer / Event Ring

**What it is:** Allow components to record events that observers can consume asynchronously, without coupling the publisher to any specific consumer.

**Where used:**

- `src/core/DeliveryEvent.hpp` — `struct DeliveryEvent` with `enum class DeliveryEventKind`:
  `SEND_OK`, `SEND_FAIL`, `ACK_RECEIVED`, `ACK_TIMEOUT`, `DUPLICATE_DROP`, `EXPIRY_DROP`, `RETRY_FIRED`, `MISROUTE_DROP`

- `src/core/DeliveryEventRing.hpp` — class `DeliveryEventRing`
  - `push(const DeliveryEvent& ev)` — called by DeliveryEngine internals; overwrites oldest if full
  - `pop(DeliveryEvent& out)` — caller drains one event at a time
  - `size()` — non-blocking count

`DeliveryEngine` owns one `DeliveryEventRing m_events` instance. Components push events into it; application code drains it via `DeliveryEngine::drain_events()`.

**Why:** The project cannot use callbacks or function pointers (Power of 10 Rule 9) and cannot use `std::function` or STL (F-Prime no-STL rule). A fixed-capacity ring with push/pop satisfies REQ-7.2.5 (bounded observability event ring) without dynamic allocation, blocking, or virtual dispatch. The pull model (consumer calls `pop()`) avoids the need for any observer registration mechanism.

---

### 2.3 Chain of Responsibility

**What it is:** Pass a request through a sequence of handlers, each of which either processes it or forwards it to the next handler.

**Where used:**

**Outbound path:**
```
DeliveryEngine::send()
  → expiry check, message_id assignment
  → Serializer::serialize()
  → TransportInterface::send_message()  (backend-specific)
    → ImpairmentEngine::process_outbound()  (loss / dup / delay)
      → ImpairmentEngine::collect_deliverable()
        → raw socket / TLS send  (via ISocketOps / IMbedtlsOps)
```

**Inbound path:**
```
raw socket / TLS receive
  → TransportInterface::receive_message()  (backend-specific framing)
    → source address validation  (REQ-6.1.11 / REQ-6.2.4)
      → Serializer::deserialize()
        → DeliveryEngine::receive()
          → ReassemblyBuffer::ingest()  (fragment reassembly)
            → OrderingBuffer  (sequence ordering)
              → DuplicateFilter::check_and_record()
                → ACK generation
                  → delivery to application
```

Key files: `src/core/DeliveryEngine.hpp/.cpp`, `src/core/Serializer.hpp/.cpp`, `src/platform/ImpairmentEngine.hpp/.cpp`, `src/platform/TcpBackend.hpp`, `src/platform/TlsTcpBackend.hpp`, `src/platform/UdpBackend.hpp`, `src/platform/DtlsUdpBackend.hpp`.

**Why:** Each stage in the message path has a single, well-defined responsibility (serialize, impair, frame, validate, dedup, order, reassemble). Chain of Responsibility maps each stage to one class, making each stage independently testable, and makes the overall message path readable top-to-bottom. It also enforces the layering rule (App → Core → Platform) at the code level.

---

### 2.4 Non-Virtual Interface (NVI)

**What it is:** Provide a non-pure virtual base method with a default implementation that subclasses can override selectively, rather than forcing every subclass to implement it.

**Where used:**

`src/core/TransportInterface.hpp` — `virtual Result register_local_id(NodeId id)`

The base class provides a no-op default that returns `OK`. Backends override it only when they need to send a HELLO frame on-wire:

| Backend | Override behaviour |
|---|---|
| `TcpBackend` (client mode) | Sends a HELLO frame so the server registers the NodeId |
| `TlsTcpBackend` (client mode) | Same |
| `UdpBackend` | Sends a HELLO datagram to the configured peer |
| `DtlsUdpBackend` (client mode) | Same |
| `LocalSimHarness` | Inherits the no-op default |

Called automatically by `DeliveryEngine::init()` immediately after the transport is initialised (REQ-6.1.10).

**Why:** `LocalSimHarness` has no HELLO concept; forcing it to implement `register_local_id()` would be meaningless. NVI avoids empty overrides while still allowing the delivery engine to call a single uniform method on any transport.

---

## 3. Creational Patterns

### 3.1 Dependency Injection

**What it is:** Supply an object's dependencies from the outside (via constructor parameters) rather than having the object create them internally, so tests can substitute fakes.

**Where used:**

Each backend exposes two constructors:

| Backend | Default constructor | Injection constructor |
|---|---|---|
| `src/platform/TcpBackend.hpp` | `TcpBackend()` — uses `SocketOpsImpl` singleton | `TcpBackend(ISocketOps& ops)` |
| `src/platform/TlsTcpBackend.hpp` | `TlsTcpBackend()` — uses `SocketOpsImpl` singleton | `TlsTcpBackend(ISocketOps& sock_ops)` |
| `src/platform/UdpBackend.hpp` | `UdpBackend()` — uses `SocketOpsImpl` singleton | `UdpBackend(ISocketOps& ops)` |
| `src/platform/DtlsUdpBackend.hpp` | `DtlsUdpBackend()` — uses both singletons | `DtlsUdpBackend(ISocketOps&, IMbedtlsOps&)` |

Test files (e.g. `tests/test_TlsTcpBackend.cpp`, `tests/test_DtlsUdpBackend.cpp`) use the injection constructors to inject mock implementations that simulate socket errors, connection failures, and TLS faults.

**Why:** Power of 10 Rule 3 bans dynamic allocation after init, ruling out runtime factory patterns. Constructor injection is the only MISRA C++:2023-compliant way to supply test doubles at build time without link-time substitution or `#ifdef` guards. It also satisfies VVP-001 §4.3 (M5 fault injection requirement for dependency-failure branches).

---

### 3.2 Singleton (Safety Variant)

**What it is:** Ensure exactly one instance of a class exists and provide global access to it — adapted here to comply with the no-heap-after-init rule.

**Where used:**

`src/core/AbortResetHandler.hpp` — class `AbortResetHandler : public IResetHandler`

- `static AbortResetHandler& instance()` returns a reference to `static AbortResetHandler s_instance` (Meyers singleton, no heap allocation).
- Registered at startup via `assert_state::set_reset_handler(AbortResetHandler::instance())`.
- Called by `NEVER_COMPILED_OUT_ASSERT` when an assertion fails in production builds.

**Why:** The reset handler must be available across the entire process lifetime, including during FATAL assertion handling. A Meyers singleton with a static local variable satisfies this without dynamic allocation (Power of 10 Rule 3) and without a global mutable pointer (MISRA C++:2023 guidance on global state). The `IResetHandler` base class keeps it testable — tests can register a different handler without touching the singleton.

---

### 3.3 Slot Pool

**What it is:** Pre-allocate a fixed array of reusable slots at init time; mark each slot as FREE or IN_USE; never call `malloc`/`new` at runtime.

**Where used:**

| Component | File | Array | Capacity constant |
|---|---|---|---|
| `AckTracker` | `src/core/AckTracker.hpp` | `Entry m_slots[ACK_TRACKER_CAPACITY]` | `ACK_TRACKER_CAPACITY = 32` |
| `RetryManager` | `src/core/RetryManager.hpp` | `RetryEntry m_slots[ACK_TRACKER_CAPACITY]` | `ACK_TRACKER_CAPACITY = 32` |
| `ReassemblyBuffer` | `src/core/ReassemblyBuffer.hpp` | `ReassemblySlot m_slots[REASSEMBLY_SLOT_COUNT]` | `REASSEMBLY_SLOT_COUNT` |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.hpp` | `DelayEntry m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` | `IMPAIR_DELAY_BUF_SIZE = 64` |
| `TcpBackend` | `src/platform/TcpBackend.hpp` | `int m_client_fds[MAX_TCP_CONNECTIONS]` | `MAX_TCP_CONNECTIONS = 8` |
| `TlsTcpBackend` | `src/platform/TlsTcpBackend.hpp` | `mbedtls_ssl_context m_ssl[MAX_TCP_CONNECTIONS]` | `MAX_TCP_CONNECTIONS = 8` |

All capacity constants are defined in `src/core/Types.hpp` and documented with rationale in `docs/CAPACITY_REFERENCE.md`.

**Why:** Power of 10 Rule 3 prohibits dynamic allocation after init. Slot pools are the standard aerospace pattern for bounded, deterministic resource management. Fixed-size pools also make WCET analysis tractable (`docs/WCET_ANALYSIS.md`) because the dominant operation count is O(capacity), which is a compile-time constant.

---

## 4. Data and Infrastructure Patterns

### 4.1 Value Object

**What it is:** A small, self-contained struct that carries all the data it needs, is copied by value, has no identity beyond its field values, and is never heap-allocated on critical paths.

**Where used:**

`src/core/MessageEnvelope.hpp` — `struct MessageEnvelope`

Key fields: `message_id`, `timestamp_us`, `expiry_time_us`, `source_id`, `destination_id`, `payload_length`, `sequence_num`, `message_type`, `priority`, `fragment_index`, `fragment_count`, `payload[MSG_MAX_PAYLOAD_BYTES]`.

Helper free functions (all in the same header): `envelope_init()`, `envelope_copy()`, `envelope_valid()`, `envelope_is_data()`, `envelope_addressed_to()`, `envelope_make_ack()`.

**Why:** The envelope must flow through every layer (application → DeliveryEngine → Serializer → transport → wire) without allocation. Copying by value is safe at every layer boundary and eliminates lifetime/ownership concerns that would otherwise require `shared_ptr` (banned) or raw pointer management (risky). The self-contained layout also makes `Serializer::serialize()` straightforward — it reads directly from the struct's fields.

---

### 4.2 Serializer

**What it is:** A stateless class with pure encode/decode functions that convert between an in-memory struct and a portable wire format.

**Where used:**

`src/core/Serializer.hpp` — class `Serializer`

- `static Result serialize(const MessageEnvelope& env, uint8_t* buf, uint32_t buf_len, uint32_t& out_len)`
- `static Result deserialize(const uint8_t* buf, uint32_t buf_len, MessageEnvelope& env)`
- Wire header size: `WIRE_HEADER_SIZE = 52U` bytes.
- Wire format carries a protocol version byte and a 2-byte frame magic (REQ-3.2.8); deserializer rejects mismatched versions and magic values.
- All multi-byte fields written/read with explicit `write_u16()` / `read_u32()` helpers for deterministic, endian-safe encoding.

**Why:** Serialization is safety-critical (classified SC with HAZ-001, HAZ-005 — see `docs/HAZARD_ANALYSIS.md §3`). Keeping it stateless and free of dependencies makes it independently unit-testable, amenable to MC/DC analysis (`docs/MCDC_ANALYSIS.md`), and straightforward to prove correct under Frama-C (a planned Class B obligation). Static methods eliminate the need for an instance and make the dependency graph acyclic.

---

### 4.3 Ring Buffer (SPSC Lock-Free)

**What it is:** A single-producer single-consumer circular buffer that uses `std::atomic` head/tail indices for lock-free operation — no mutex, no blocking.

**Where used:**

`src/core/RingBuffer.hpp` — template class `RingBuffer<T, N>`

- `std::atomic<uint32_t> m_head` — written by producer (release), read by consumer (acquire).
- `std::atomic<uint32_t> m_tail` — written by consumer (release), read by producer (acquire).
- `push(const MessageEnvelope& env)` — returns `ERR_FULL` if ring is full (never blocks).
- `pop(MessageEnvelope& env)` — returns `ERR_EMPTY` if no data (never blocks).

Used inside each backend's inbound queue to decouple the receive-thread from the `DeliveryEngine::receive()` call.

**Why:** The project cannot use `std::mutex` or `std::thread` (F-Prime no-STL rule), and cannot use GCC `__atomic` built-ins (banned by `CLAUDE.md §1d`). `std::atomic` is the one STL feature explicitly permitted as a carve-out (`.claude/CLAUDE.md §3`) because it maps directly to a hardware primitive, has no dynamic allocation, and is endorsed by MISRA C++:2023 for lock-free shared state. The SPSC pattern avoids the ABA problem that affects multi-producer/consumer rings and keeps the proof of correctness simple.

---

### 4.4 RAII Lifecycle

**What it is:** All resource acquisition happens in an explicit `init()` call; all resource release happens in a matching `close()` call or destructor. Constructors only zero-initialize.

**Where used** (representative list):

| Component | File | `init()` | `close()` |
|---|---|---|---|
| `DeliveryEngine` | `src/core/DeliveryEngine.hpp` | `init(TransportInterface*, ChannelConfig, NodeId)` | implicit (no heap) |
| `TcpBackend` | `src/platform/TcpBackend.hpp` | `init(const TransportConfig&)` | `close()` |
| `TlsTcpBackend` | `src/platform/TlsTcpBackend.hpp` | `init(const TransportConfig&)` | `close()` |
| `UdpBackend` | `src/platform/UdpBackend.hpp` | `init(const TransportConfig&)` | `close()` |
| `DtlsUdpBackend` | `src/platform/DtlsUdpBackend.hpp` | `init(const TransportConfig&)` | `close()` |
| `AckTracker` | `src/core/AckTracker.hpp` | `init()` | none needed |
| `RetryManager` | `src/core/RetryManager.hpp` | `init()` | none needed |
| `RingBuffer` | `src/core/RingBuffer.hpp` | `init()` | none needed |
| `DeliveryEventRing` | `src/core/DeliveryEventRing.hpp` | `init()` | none needed |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.hpp` | `init(const ImpairmentConfig&)` | none needed |

**Why:** Power of 10 Rule 3 requires all allocation to occur during an explicit init phase. Separating construction (zero-init) from initialisation (`init()`) makes the init-phase boundary unambiguous — any resource acquired after `init()` returns is a violation. It also makes components reset-safe: calling `init()` again returns a component to a known-good state, which is important for the controlled-reset path triggered by `NEVER_COMPILED_OUT_ASSERT` in production builds.

---

## 5. Patterns Deliberately Absent

| Pattern | Reason absent |
|---|---|
| **Factory / Abstract Factory** | Direct instantiation is sufficient; the Strategy + DI combination already decouples construction from use |
| **Decorator / Proxy** | Composition and delegation via the Chain of Responsibility are preferred; Decorator would require virtual wrapping that adds vtable overhead for no benefit |
| **Visitor** | Message type dispatch is explicit `switch`/`if` on `MessageType` enum — statically analysable and MISRA-compliant; Visitor would introduce a second dispatch mechanism and a mutual dependency between the visitor and each message type |
| **Iterator** | All collections are fixed-size arrays with explicit index loops — directly verifiable against Power of 10 Rule 2 (fixed loop bounds); an iterator abstraction adds no value and obscures the bound |
| **Template Method** | NVI covers the one case where a base class needs to define a skeleton with optional override; a heavier Template Method hierarchy is not warranted |

---

*This document is manually maintained. Update it when a new pattern is introduced or an existing one is significantly refactored.*
