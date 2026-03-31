# UC_28 — DeliveryEngine::init(): DuplicateFilter, AckTracker, RetryManager all reset
**HL Group:** HL-16 — System Initialization
**Actor:** System
**Requirement traceability:** REQ-3.2.1, REQ-3.2.4, REQ-3.2.5, REQ-3.2.6, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3

---

## 1. Use Case Overview

### What triggers this flow

The system initialization routine calls `DeliveryEngine::init()` after constructing a
`TransportInterface`-backed transport and populating a `ChannelConfig`. This is always an
initialization-phase call, before any `send()` or `receive()` calls.

### Expected outcome (single goal)

All four owned sub-components (`AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`)
are fully initialized to a known-clean state, `m_initialized == true`, and the engine is ready
for `send()`/`receive()` calls. No messages have been sent or received. No dynamic allocation
occurs after this function returns.

---

## 2. Entry Points

Primary entry point:

```
void DeliveryEngine::init(
    TransportInterface* transport,   // [in] non-owning pointer to initialized transport
    const ChannelConfig& cfg,        // [in] channel configuration (copied)
    NodeId local_id                  // [in] this node's ID (must not be NODE_ID_INVALID)
)
```
Defined: src/core/DeliveryEngine.cpp, lines 17–51.

Sub-component entry points called transitively:

| Function | File | Line |
|----------|------|------|
| `AckTracker::init()` | src/core/AckTracker.cpp | 23 |
| `RetryManager::init()` | src/core/RetryManager.cpp | 46 |
| `DuplicateFilter::init()` | src/core/DuplicateFilter.cpp | 23 |
| `MessageIdGen::init(seed)` | src/core/MessageId.hpp | 36 (inline) |

---

## 3. End-to-End Control Flow (Step-by-Step)

**Step 1 — Precondition assertions** (DeliveryEngine.cpp:21–22)
```
NEVER_COMPILED_OUT_ASSERT(transport != nullptr)
NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID)
```
Both are unconditional (not gated on NDEBUG). A null `transport` or `local_id == 0` terminates
the program in all build configurations.

**Step 2 — Member field assignments** (DeliveryEngine.cpp:24–27)
```
m_transport   = transport
m_cfg         = cfg          // full struct copy; caller's cfg not retained
m_local_id    = local_id
m_initialized = false        // set false BEFORE sub-component inits
```
`m_initialized` is explicitly set to `false` first. Any concurrent read of `m_initialized` in
this window (before Step 9) would see `false` and correctly reject the engine as unready.

**Step 3 — `m_ack_tracker.init()`** (AckTracker.cpp:23)
- `memset(m_slots, 0, sizeof(m_slots))` — zeroes the entire `ACK_TRACKER_CAPACITY` (32) slot
  array including all `MessageEnvelope` payloads embedded in each `Entry`.
- `m_count = 0U`.
- `NEVER_COMPILED_OUT_ASSERT(m_count == 0U)`.
- Verification loop bounded by `ACK_TRACKER_CAPACITY` (32): for each slot,
  `NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::FREE)`. Verifies that
  `EntryState::FREE == 0` holds on this platform. If it does not, the assertion fires
  unconditionally.
- Returns `void`.

**Step 4 — `m_retry_manager.init()`** (RetryManager.cpp:46)
- `m_count = 0U`.
- `m_initialized = true` (RetryManager has its own init flag).
- Initialization loop bounded by `ACK_TRACKER_CAPACITY` (32): for each slot explicitly
  assigns:
  - `m_slots[i].active = false`
  - `m_slots[i].retry_count = 0U`
  - `m_slots[i].backoff_ms = 0U`
  - `m_slots[i].next_retry_us = 0ULL`
  - `m_slots[i].expiry_us = 0ULL`
  - `m_slots[i].max_retries = 0U`
  - `envelope_init(m_slots[i].env)` — zero-fills the embedded `MessageEnvelope` and sets
    `message_type = INVALID`.
  NOTE: `RetryManager::init()` does NOT use `memset`; it initializes each field explicitly.
  Safer against struct-padding pitfalls but O(`ACK_TRACKER_CAPACITY`) instead of O(1).
- `NEVER_COMPILED_OUT_ASSERT(m_count == 0U)`.
- `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
- Returns `void`.

**Step 5 — `m_dedup.init()`** (DuplicateFilter.cpp:23)
- `memset(m_window, 0, sizeof(m_window))` — zeroes the `DEDUP_WINDOW_SIZE` (128) entry
  sliding window array.
- `m_next = 0U`; `m_count = 0U`.
- `NEVER_COMPILED_OUT_ASSERT(m_next == 0U)`.
- `NEVER_COMPILED_OUT_ASSERT(m_count == 0U)`.
- Returns `void`.

**Step 6 — Seed computation** (DeliveryEngine.cpp:36–39)
```
uint64_t id_seed = static_cast<uint64_t>(local_id)
if (id_seed == 0ULL): id_seed = 1ULL    // belt-and-suspenders guard
```
Since `local_id != NODE_ID_INVALID` (= 0U) was asserted in Step 1, `id_seed` is always >= 1
here. The `if` is a redundant defensive guard.

**Step 7 — `m_id_gen.init(id_seed)`** (MessageId.hpp:36 — inline)
```
NEVER_COMPILED_OUT_ASSERT(seed != 0ULL)
m_next = seed
NEVER_COMPILED_OUT_ASSERT(m_next == seed)
```
After this call, `m_id_gen.next()` will return `local_id` as the first message ID, then
`local_id + 1`, `local_id + 2`, etc., skipping 0 on wraparound.

**Step 8 — `m_initialized = true`** (DeliveryEngine.cpp:42)
Engine is now fully ready.

**Step 9 — Post-condition assertions** (DeliveryEngine.cpp:45–46)
```
NEVER_COMPILED_OUT_ASSERT(m_initialized)
NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)
```

**Step 10 — Logger call** (DeliveryEngine.cpp:48–50)
```
Logger::log(Severity::INFO, "DeliveryEngine",
            "Initialized channel=%u, local_id=%u",
            cfg.channel_id, local_id)
```
The only external call during `init()`. Logs the channel ID (from `cfg`) and the local node
ID. No return value check (Logger::log is expected non-failing).

**Step 11 — Return `void`** to caller.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::init(transport, cfg, local_id)           [DeliveryEngine.cpp:17]
 ├── NEVER_COMPILED_OUT_ASSERT(transport != nullptr)
 ├── NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID)
 ├── m_transport=transport; m_cfg=cfg; m_local_id=local_id; m_initialized=false
 │
 ├── m_ack_tracker.init()                                 [AckTracker.cpp:23]
 │    ├── memset(m_slots, 0, sizeof(m_slots))             [clears 32 Entry slots]
 │    ├── m_count = 0U
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
 │    └── [loop i=0..31]
 │         └── NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::FREE)
 │
 ├── m_retry_manager.init()                               [RetryManager.cpp:46]
 │    ├── m_count = 0U; m_initialized = true
 │    └── [loop i=0..31]
 │         ├── m_slots[i].active = false
 │         ├── m_slots[i].retry_count = 0U
 │         ├── m_slots[i].backoff_ms = 0U
 │         ├── m_slots[i].next_retry_us = 0ULL
 │         ├── m_slots[i].expiry_us = 0ULL
 │         ├── m_slots[i].max_retries = 0U
 │         └── envelope_init(m_slots[i].env)              [MessageEnvelope.hpp:47]
 │              ├── memset(&env, 0, sizeof(MessageEnvelope))
 │              └── env.message_type = MessageType::INVALID
 │    (after loop)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
 │    └── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │
 ├── m_dedup.init()                                       [DuplicateFilter.cpp:23]
 │    ├── memset(m_window, 0, sizeof(m_window))           [clears 128 Entry slots]
 │    ├── m_next = 0U; m_count = 0U
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_next == 0U)
 │    └── NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
 │
 ├── id_seed = static_cast<uint64_t>(local_id)
 │    └── [if id_seed == 0: id_seed = 1ULL]               [defensive; unreachable after Step 1 assert]
 │
 ├── m_id_gen.init(id_seed)                               [MessageId.hpp:36 inline]
 │    ├── NEVER_COMPILED_OUT_ASSERT(seed != 0ULL)
 │    ├── m_next = seed
 │    └── NEVER_COMPILED_OUT_ASSERT(m_next == seed)
 │
 ├── m_initialized = true
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)
 └── Logger::log(INFO, "DeliveryEngine", "Initialized channel=%u, local_id=%u", ...)
```

---

## 5. Key Components Involved

| Component | File(s) | Role |
|-----------|---------|------|
| `DeliveryEngine` | src/core/DeliveryEngine.cpp / .hpp | Coordinator. Owns all four sub-components as inline value members. `init()` is the sole entry point for transitioning the engine to ready state. |
| `AckTracker` | src/core/AckTracker.cpp / .hpp | Tracks outstanding messages awaiting ACK. Fixed array of `ACK_TRACKER_CAPACITY=32` slots with FREE/PENDING/ACKED state machine. `init()` zeroes slots via `memset` + verification loop. |
| `RetryManager` | src/core/RetryManager.cpp / .hpp | Schedules messages for retry with exponential backoff. Fixed array of `ACK_TRACKER_CAPACITY=32` slots. Has its own `m_initialized` flag. `init()` initializes each field explicitly (no `memset`). |
| `DuplicateFilter` | src/core/DuplicateFilter.cpp / .hpp | Sliding-window dedup suppression. Fixed array of `DEDUP_WINDOW_SIZE=128` entries with round-robin eviction. `init()` zeroes window via `memset`. |
| `MessageIdGen` | src/core/MessageId.hpp (inline) | Incrementing counter seeded with `local_id`. Generates unique non-zero message IDs per sender. `init(seed)` sets `m_next = seed`. |
| `TransportInterface*` | src/core/TransportInterface.hpp | Non-owning raw pointer stored as `m_transport`. NOT called during `init()`. Lifetime managed by caller. |
| `ChannelConfig` | src/core/ChannelConfig.hpp | Passed by `const` reference; copied into `m_cfg` by value. Caller's `cfg` is not referenced after `init()` returns. |
| `Logger` | src/core/Logger.hpp | Emits one `INFO` message at the end of successful `init()`. The only external call during init. |
| `envelope_init()` | src/core/MessageEnvelope.hpp:47 | Called 32 times in `RetryManager::init()` — once per slot. Zero-fills the inline `MessageEnvelope`. |

---

## 6. Branching Logic / Decision Points

| Branch | Condition | True path | False path |
|--------|-----------|-----------|------------|
| `transport == nullptr` | `transport == nullptr` | `NEVER_COMPILED_OUT_ASSERT` fires; program terminates | `m_transport = transport`; continue |
| `local_id == NODE_ID_INVALID` | `local_id == 0U` | `NEVER_COMPILED_OUT_ASSERT` fires; program terminates | Continue |
| Seed coercion | `id_seed == 0ULL` | `id_seed = 1ULL` (defensive; unreachable after the assert above) | `id_seed = static_cast<uint64_t>(local_id)` |
| AckTracker per-slot assertion | `m_slots[i].state != EntryState::FREE` | `NEVER_COMPILED_OUT_ASSERT` fires; indicates `EntryState::FREE != 0` on this platform | All 32 slots confirmed FREE; loop completes |
| RetryManager slot loop | `i < ACK_TRACKER_CAPACITY` | Explicit per-field zeroing + `envelope_init()` | Loop ends at i=32 |
| DuplicateFilter window loop | (implicit in `memset`) | All 128 entries zeroed | n/a |
| `m_next == 0` wraparound in `MessageIdGen::next()` | Only relevant at runtime, not during `init()` | `m_next = 1ULL` | Increment normally |

---

## 7. Concurrency / Threading Behavior

`DeliveryEngine::init()` is intended to be called exactly once during system initialization,
before any concurrent access begins. The code contains no locks, mutexes, or atomics in the
init path.

`AckTracker`, `RetryManager`, and `DuplicateFilter` all use plain (non-atomic) fields. None
are thread-safe for concurrent calls.

`init()` must complete (happens-before) any thread calling `send()` or `receive()`. No
synchronization is provided within `init()` itself.

If `init()` is called a second time (re-init scenario), `m_initialized` is set to `false` at
Step 2 (line 27), making the engine temporarily non-functional during re-init. Any concurrent
read of `m_initialized` in this window observes `false`. No re-init guard exists. See Risk 4.

The `RingBuffer` (used by `LocalSimHarness`) uses `std::atomic<uint32_t>` for SPSC thread
safety, but `RingBuffer` is not touched during `DeliveryEngine::init()`.

---

## 8. Memory & Ownership Semantics

All storage is statically allocated as inline value members inside the `DeliveryEngine` object.
No dynamic (heap) allocation occurs anywhere in the init path (Power of 10 Rule 3).

| Member | Type | Storage size (approx.) |
|--------|------|------------------------|
| `m_ack_tracker` | `AckTracker` (value) | `32 × (sizeof(MessageEnvelope) + 8 + 1)` ≈ 132KB |
| `m_retry_manager` | `RetryManager` (value) | `32 × (sizeof(MessageEnvelope) + 32)` ≈ 132KB |
| `m_dedup` | `DuplicateFilter` (value) | `128 × (8 + 8 + 1)` ≈ 2.2KB |
| `m_id_gen` | `MessageIdGen` (value) | 8 bytes (`m_next`) |
| `m_transport` | `TransportInterface*` | 8 bytes (non-owning pointer) |
| `m_cfg` | `ChannelConfig` (value) | ~100 bytes |
| `m_local_id` | `NodeId` (uint32_t) | 4 bytes |
| `m_initialized` | `bool` | 1 byte |

`m_transport`: non-owning raw pointer. `DeliveryEngine` does not `delete` it. Lifetime
management is the caller's responsibility.

`m_cfg`: copied from `cfg` parameter at line 25 (`m_cfg = cfg`). After `init()` returns,
`m_cfg` is independent of the caller's struct. No pointer to the caller's `cfg` is retained.

`memset` usage:
- `AckTracker::init()`: `memset` on `m_slots`. Correct as long as `EntryState::FREE == 0`
  (confirmed: AckTracker.hpp:73 defines `FREE = 0U`).
- `DuplicateFilter::init()`: `memset` on `m_window`. Relies on `valid=false` when all
  bytes are `0x00`. Valid per C++ standard for `bool`.
- `RetryManager::init()`: avoids `memset`; initializes each field explicitly, which is
  safer against struct-padding pitfalls.

---

## 9. Error Handling Flow

`DeliveryEngine::init()` is declared `void`; it returns no `Result`.

All error detection is via `NEVER_COMPILED_OUT_ASSERT()` (unconditional, not gated on NDEBUG).
Assertion failures terminate the program in all build configurations.

| Condition | Location | Effect |
|-----------|----------|--------|
| `transport == nullptr` | DeliveryEngine.cpp:21 | Assert fires; program terminates |
| `local_id == NODE_ID_INVALID` | DeliveryEngine.cpp:22 | Assert fires; program terminates |
| `AckTracker` slot != FREE after `memset` | AckTracker.cpp:36 (per-slot loop) | Assert fires; `EntryState::FREE != 0` on this platform |
| `MessageIdGen` seed == 0 | MessageId.hpp:39 | Assert fires; seed coercion at line 37-39 prevents this in practice |
| `m_initialized` false at post-condition | DeliveryEngine.cpp:45 | Assert fires; should never occur |

Sub-component inits are all `void` with no return-value error paths. No error recovery is
possible during init; the system must be restarted. This is consistent with the F-Prime `FATAL`
error model for init-phase failures.

`Logger::log()` at the end is informational only; a failure there does not roll back init.

---

## 10. External Interactions

`TransportInterface*`: the pointer is stored but the transport is NOT called during `init()`.
No network I/O occurs.

`Logger::log()`: called once at the end of `DeliveryEngine::init()` with `Severity::INFO`.
Outputs `channel_id` and `local_id` values. This is the only external call during init.

`memset` (`<cstring>`): called twice — once in `AckTracker::init()` and once in
`DuplicateFilter::init()`. C standard library, not OS calls.

No file I/O, no socket interactions, no OS calls.

---

## 11. State Changes / Side Effects

Before `init()`: all members are indeterminate (no default constructor defined for
`DeliveryEngine`).

After `init()` completes:

| Member | State after init() |
|--------|--------------------|
| `m_transport` | `= transport` (caller-provided pointer) |
| `m_cfg` | `= copy of cfg` parameter |
| `m_local_id` | `= local_id` |
| `m_initialized` | `= true` |
| `m_ack_tracker.m_slots[0..31].state` | `= EntryState::FREE` (zeroed) |
| `m_ack_tracker.m_count` | `= 0` |
| `m_retry_manager.m_slots[0..31].active` | `= false` |
| `m_retry_manager.m_slots[0..31]` (all scalar fields) | `= 0` / INVALID |
| `m_retry_manager.m_count` | `= 0` |
| `m_retry_manager.m_initialized` | `= true` |
| `m_dedup.m_window[0..127].valid` | `= false` (zeroed) |
| `m_dedup.m_next` | `= 0` |
| `m_dedup.m_count` | `= 0` |
| `m_id_gen.m_next` | `= id_seed` (= `(uint64_t)local_id`, minimum 1) |

Side effect: `Logger::log()` emits one `INFO` message with `channel_id` and `local_id`.

---

## 12. Sequence Diagram

```
Caller (main() or test harness)
  |
  |-- init(transport, cfg, local_id) --------------> DeliveryEngine::init()
  |                                                     |
  |                                                     |-- NEVER_COMPILED_OUT_ASSERT(transport != nullptr)
  |                                                     |-- NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID)
  |                                                     |-- m_transport=transport; m_cfg=cfg
  |                                                     |-- m_local_id=local_id; m_initialized=false
  |                                                     |
  |                                                     |-- m_ack_tracker.init() ----------> AckTracker::init()
  |                                                     |                                       |-- memset(m_slots, 0)
  |                                                     |                                       |-- m_count = 0
  |                                                     |                                       |-- [loop i=0..31]
  |                                                     |                                       |     assert slots[i].state == FREE
  |                                                     |                                   <--
  |                                                     |
  |                                                     |-- m_retry_manager.init() -------> RetryManager::init()
  |                                                     |                                       |-- m_count=0; m_initialized=true
  |                                                     |                                       |-- [loop i=0..31]
  |                                                     |                                       |     zero all fields
  |                                                     |                                       |     envelope_init(slots[i].env)
  |                                                     |                                       |-- assert m_count==0; assert m_initialized
  |                                                     |                                   <--
  |                                                     |
  |                                                     |-- m_dedup.init() ---------------> DuplicateFilter::init()
  |                                                     |                                       |-- memset(m_window, 0)
  |                                                     |                                       |-- m_next=0; m_count=0
  |                                                     |                                       |-- assert m_next==0; assert m_count==0
  |                                                     |                                   <--
  |                                                     |
  |                                                     |-- id_seed = (uint64_t)local_id
  |                                                     |
  |                                                     |-- m_id_gen.init(id_seed) -------> MessageIdGen::init() [inline]
  |                                                     |                                       |-- assert seed != 0
  |                                                     |                                       |-- m_next = seed
  |                                                     |                                       |-- assert m_next == seed
  |                                                     |                                   <--
  |                                                     |
  |                                                     |-- m_initialized = true
  |                                                     |-- NEVER_COMPILED_OUT_ASSERT(m_initialized)
  |                                                     |-- NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)
  |                                                     |-- Logger::log(INFO, "Initialized channel=%u, local_id=%u")
  |                                                     |
  |<-- (void) <-----------------------------------------
```

---

## 13. Initialization vs Runtime Flow

### Startup / initialization (this use case)

`DeliveryEngine::init()` is the sole init-phase entry point. All four sub-components are
initialized in fixed order: `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`.
No messages are sent or received. All storage is pre-allocated as inline value members. After
`init()` returns, `m_initialized == true`.

### Steady-state runtime (post-init; not covered here)

- `send()` uses `m_id_gen.next()` to assign message IDs; `m_ack_tracker.track()` and
  `m_retry_manager.schedule()` for reliable classes.
- `receive()` uses `m_dedup.check_and_record()` for `RELIABLE_RETRY` dedup; calls
  `m_ack_tracker.on_ack()` and `m_retry_manager.on_ack()` on ACK receipt.
- `pump_retries()` drives `m_retry_manager.collect_due()` + `send_via_transport()`.
- `sweep_ack_timeouts()` drives `m_ack_tracker.sweep_expired()`.

Any runtime call that reaches `send()` or `receive()` before a successful `init()` will fail
the `NEVER_COMPILED_OUT_ASSERT(m_initialized)` guard at DeliveryEngine.cpp:79 / 153.
`RetryManager` carries its own `NEVER_COMPILED_OUT_ASSERT(m_initialized)` in every method.

---

## 14. Known Risks / Observations

### Risk 1 — No return value from `init()`
The function is declared `void`. There is no way for the caller to distinguish between a
successful initialization and one that was intercepted by a `NEVER_COMPILED_OUT_ASSERT`.
If the assert fires, the program terminates; there is no partial-success path to observe.

### Risk 2 — `memset` reliance on zero-equals-FREE assumption
`AckTracker::init()` and `DuplicateFilter::init()` use `memset` and then assert the resulting
state. If `EntryState::FREE != 0` or the `valid` bool field has a non-zero `false`
representation on a given platform, this is undefined behavior. The verification loop in
`AckTracker` guards against this in all builds (NEVER_COMPILED_OUT_ASSERT unconditional).

### Risk 3 — Implicit ordering of sub-component inits
The code initializes `AckTracker`, `RetryManager`, `DuplicateFilter`, then `MessageIdGen` in
that order. There is no formal dependency between them; any order would be correct. A future
developer adding a cross-dependency could introduce a subtle ordering bug.

### Risk 4 — `m_initialized = false` at entry enables reentrancy hazard
If `init()` is called a second time on an already-initialized engine, `m_initialized` is set
to `false` at Step 2 (line 27). Any concurrent thread observing `m_initialized` in this window
sees `false`. No re-init guard exists; no lock prevents concurrent `send()`/`receive()` during
re-init.

### Risk 5 — `ACK_TRACKER_CAPACITY` shared between `AckTracker` and `RetryManager`
Both sub-components share the same capacity constant (32). This is intentional but means the
retry table is identically sized to the ACK tracker. If retry load significantly differs from
ACK tracking load, one table may be over- or under-sized without a compile-time warning.

### Observation — `envelope_init()` called 32 times in `RetryManager::init()`
Each call to `envelope_init()` executes `memset(&env, 0, sizeof(MessageEnvelope))` where
`sizeof(MessageEnvelope) ≈ 4140 bytes`. Total zeroed in `RetryManager::init()`: ~130KB.
At current capacity of 32 this is negligible for initialization, but is O(N) in capacity.

---

## 15. Unknowns / Assumptions

**[CONFIRMED-1]** `NODE_ID_INVALID = 0U` from src/core/Types.hpp. The assertion at
DeliveryEngine.cpp:22 guards against `local_id == 0`.

**[CONFIRMED-2]** `ACK_TRACKER_CAPACITY = 32U`, `DEDUP_WINDOW_SIZE = 128U` from
src/core/Types.hpp.

**[CONFIRMED-3]** `EntryState::FREE = 0U` defined at AckTracker.hpp:73. The `memset` zeroing
in `AckTracker::init()` is correct and the verification loop will always find `FREE` after
`memset` on a conforming platform.

**[CONFIRMED-4]** `DuplicateFilter::Entry.valid` is a `bool`. Standard C++ guarantees `false`
is represented as `0x00`, so `memset` to 0 correctly initializes `valid = false`.

**[CONFIRMED-5]** `MessageIdGen::init(seed)` is inline in MessageId.hpp:36. It stores `seed`
in `m_next`. The first call to `next()` returns `seed` (= `local_id`), not `seed + 1`.

**[CONFIRMED-6]** `RetryManager::init()` does NOT use `memset`. It initializes each of the 32
slots by explicit field assignment plus `envelope_init()`. This is confirmed from source
inspection (RetryManager.cpp:46).

**[ASSUMPTION-A]** `Logger::log()` is non-blocking, does not allocate on the critical path,
and always returns without error. No return value is checked.

**[ASSUMPTION-B]** Re-initialization (calling `init()` more than once on the same object) is
mechanically supported (all state is re-zeroed) but is not documented or tested as an intended
use pattern. The `RISK-4` reentrancy hazard applies in this scenario.
