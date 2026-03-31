# UC_30 — LocalSimHarness round-trip: message sent, impaired, and received in one process

**HL Group:** HL-14 — In-Process Simulation
**Actor:** User
**Requirement traceability:** REQ-4.1.2, REQ-4.1.3, REQ-5.3.2

---

## 1. Use Case Overview

### What triggers this flow

A test function (e.g., `test_basic_send_receive()`) constructs two `LocalSimHarness`
objects on the stack, initializes both, links one to the other via `link()`, builds a
DATA envelope, calls `send_message()` on the sender, and then calls `receive_message()`
on the receiver. No real network exists.

### Expected outcome (single goal)

The receiver's `receive_message()` returns `Result::OK` and the received
`MessageEnvelope` has identical `message_id`, `source_id`, `destination_id`,
`payload_length`, and payload bytes as the sent envelope.

### Success outcome

`harness_b.receive_message()` returns `Result::OK` with the envelope data matching the
sent envelope. Both harnesses are closed cleanly.

### Error outcomes

- `m_peer == nullptr` in `send_message()` → `NEVER_COMPILED_OUT_ASSERT` fires (fatal);
  only occurs if `link()` was not called.
- Delay buffer full (`m_delay_count >= IMPAIR_DELAY_BUF_SIZE=32`) →
  `process_outbound()` returns `ERR_FULL`; `send_message()` propagates it.
- `RingBuffer` full in `inject()` → `push()` returns `ERR_FULL`; `WARNING_HI` logged.
- No message within `timeout_ms` → `receive_message()` returns `ERR_TIMEOUT`.

---

## 2. Entry Points

| Symbol | File | Line |
|---|---|---|
| `LocalSimHarness::init(config)` | `src/platform/LocalSimHarness.cpp` | 48 |
| `LocalSimHarness::link(peer)` | `src/platform/LocalSimHarness.cpp` | 78 |
| `LocalSimHarness::send_message(envelope)` | `src/platform/LocalSimHarness.cpp` | 109 |
| `LocalSimHarness::inject(envelope)` | `src/platform/LocalSimHarness.cpp` | 91 |
| `LocalSimHarness::receive_message(envelope, timeout_ms)` | `src/platform/LocalSimHarness.cpp` | 147 |
| `LocalSimHarness::close()` | `src/platform/LocalSimHarness.cpp` | 192 |
| `ImpairmentEngine::process_outbound(env, now_us)` | `src/platform/ImpairmentEngine.cpp` | 151 |
| `ImpairmentEngine::collect_deliverable(now_us, buf, cap)` | `src/platform/ImpairmentEngine.cpp` | 216 |
| `ImpairmentEngine::queue_to_delay_buf(env, release_us)` | `src/platform/ImpairmentEngine.cpp` | 83 |
| `RingBuffer::push(env)` | `src/core/RingBuffer.hpp` | — |
| `RingBuffer::pop(env)` | `src/core/RingBuffer.hpp` | — |
| `timestamp_now_us()` | `src/core/Timestamp.cpp` | — |

---

## 3. End-to-End Control Flow (Step-by-Step)

### Phase 1 — Object Construction

**Step 1.1** `LocalSimHarness harness_a;` constructed on the stack.
Constructor (`LocalSimHarness.cpp:27-32`):
- `m_peer = nullptr` (member initializer).
- `m_open = false` (member initializer).
- `NEVER_COMPILED_OUT_ASSERT(!m_open)` [line 31].

**Step 1.2** `LocalSimHarness harness_b;` constructed identically.

### Phase 2 — Initialize harness_a

**Step 2.1** Build a `TransportConfig` with `kind=TransportKind::LOCAL_SIM`,
`local_node_id=1`, `is_server=false`, `num_channels=0`.

**Step 2.2** `harness_a.init(cfg_a)` called (`LocalSimHarness.cpp:48`):
- `NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::LOCAL_SIM)` — passes.
- `NEVER_COMPILED_OUT_ASSERT(!m_open)` — passes.
- `m_recv_queue.init()` — resets `RingBuffer` head and tail atomics to 0.
- `impairment_config_default(imp_cfg)` — sets `imp_cfg.enabled=false`,
  `prng_seed=42ULL`.
- `if (config.num_channels > 0U)` — false (0 channels) → `imp_cfg.enabled` stays
  `false`.
- `m_impairment.init(imp_cfg)` — seeds PRNG to 42, zeroes delay and reorder buffers,
  `m_initialized=true`.
- `m_open = true`.
- `Logger::log(INFO, "LocalSimHarness", "Local simulation harness initialized (node 1)")`.
- `NEVER_COMPILED_OUT_ASSERT(m_open)` — passes.
- Returns `Result::OK`.

### Phase 3 — Initialize harness_b

Identical to Phase 2 with `local_node_id=2`. Returns `Result::OK`.

### Phase 4 — Link

**Step 4.1** `harness_a.link(&harness_b)` (`LocalSimHarness.cpp:78`):
- `NEVER_COMPILED_OUT_ASSERT(peer != nullptr)` — passes.
- `NEVER_COMPILED_OUT_ASSERT(peer != this)` — passes.
- `m_peer = &harness_b`.
- `Logger::log(INFO, "LocalSimHarness", "Harness linked to peer")`.

Note: `harness_b.m_peer` is NOT set. This is a unidirectional link.

### Phase 5 — Build the Envelope

`MessageEnvelope send_env` is constructed with `message_type=DATA`, `message_id=12345`,
`source_id=1`, `destination_id=2`, `payload="Hello"` (5 bytes),
`payload_length=5`, `expiry_time_us=0`.

### Phase 6 — Send

**Step 6.1** `harness_a.send_message(send_env)` called (`LocalSimHarness.cpp:109`):

**Step 6.1a** Precondition assertions:
- `NEVER_COMPILED_OUT_ASSERT(m_open)` — true.
- `NEVER_COMPILED_OUT_ASSERT(m_peer != nullptr)` — `&harness_b`.
- `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` — passes.

**Step 6.1b** `now_us = timestamp_now_us()` — current monotonic microseconds.

**Step 6.1c** `m_impairment.process_outbound(envelope, now_us)`:
- Precondition assertions pass.
- `m_cfg.enabled == false` → **disabled path taken**.
- Disabled path: `m_delay_count (0) >= IMPAIR_DELAY_BUF_SIZE (32)` → false → no
  `ERR_FULL`.
- Calls `queue_to_delay_buf(in_env, now_us)`:
  - Finds slot 0 inactive.
  - `envelope_copy(m_delay_buf[0].env, env)`.
  - `m_delay_buf[0].release_us = now_us`.
  - `m_delay_buf[0].active = true`.
  - `m_delay_count = 1`.
  - Returns `Result::OK`.
- `process_outbound()` returns `Result::OK`.

**Step 6.1d** `res != ERR_IO` → message not dropped; execution continues.

**Step 6.1e** `MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]` on stack.
`delayed_count = m_impairment.collect_deliverable(now_us, delayed_envelopes, 32)`:
- Loop over 32 slots: slot 0 has `active=true`, `release_us == now_us` → deliverable.
- `envelope_copy(out_buf[0], m_delay_buf[0].env)`, `out_count=1`.
- `m_delay_buf[0].active=false`, `m_delay_count=0`.
- Returns `out_count = 1`.

**Step 6.1f** Inject delayed messages into peer (loop over `delayed_count=1`):
- `m_peer->inject(delayed_envelopes[0])` → `harness_b.inject(delayed_envelopes[0])`:
  - `NEVER_COMPILED_OUT_ASSERT(m_open)` — passes.
  - `m_recv_queue.push(envelope)` → copies into `m_buf[0]`; `m_tail` → 1.
  - Returns `Result::OK`.
- **delayed_envelopes[0] (copy of send_env) is now in harness_b.m_recv_queue slot 0.**

**Step 6.1g** Inject original envelope into peer:
- `res = m_peer->inject(envelope)` → `harness_b.inject(send_env)`:
  - `m_recv_queue.push(send_env)` → copies into `m_buf[1]`; `m_tail` → 2.
  - Returns `Result::OK`.
- **send_env is now in harness_b.m_recv_queue slot 1. Queue has 2 items.**

Note on double injection: with `enabled=false`, the message goes through the delay buffer
path AND is then injected directly. This is by design and is idempotent when the receiver
only pops once.

**Step 6.1h** `NEVER_COMPILED_OUT_ASSERT(res == OK || res == ERR_FULL)` — passes.
Returns `Result::OK`.

### Phase 7 — Receive

**Step 7.1** `harness_b.receive_message(recv_env, 100U)` called
(`LocalSimHarness.cpp:147`):
- `NEVER_COMPILED_OUT_ASSERT(m_open)` — passes.
- First attempt: `res = m_recv_queue.pop(envelope)`.
  - Queue has 2 items (`tail=2`, `head=0`).
  - Copies `m_buf[0]` (copy of `send_env`) into `recv_env`; `m_head` → 1.
  - Returns `Result::OK`.
- `result_ok(res) == true` → **immediate return with `Result::OK`**.
- Polling loop (`nanosleep`) is never entered.

### Phase 8 — Verify Fields

All field comparisons pass:
- `recv_env.message_id == 12345`, `source_id == 1`, `destination_id == 2`,
  `payload_length == 5`, payload bytes `'H','e','l','l','o'`.

### Phase 9 — Teardown

`harness_a.close()` (`LocalSimHarness.cpp:192`):
- `m_peer = nullptr`.
- `m_open = false`.
- `Logger::log(INFO, "LocalSimHarness", "Transport closed")`.

`harness_b.close()` — identical. `harness_b.m_peer` was never set; setting to nullptr
is a no-op.

### Phase 10 — Stack Unwind (Destructor Execution)

`LocalSimHarness::~LocalSimHarness()` (`LocalSimHarness.cpp:38-42`) calls `close()`
again via qualified call. All fds already cleared; emits a duplicate INFO log (harmless).

---

## 4. Call Tree (Hierarchical)

```
[caller/test]
  harness_a.init(cfg_a)  [LocalSimHarness.cpp:48]
    m_recv_queue.init()                              — RingBuffer
    impairment_config_default(imp_cfg)
    m_impairment.init(imp_cfg)                       — ImpairmentEngine.cpp:44
      m_prng.seed(42ULL)                             — PrngEngine.hpp:43
      memset(m_delay_buf, 0, ...)
      memset(m_reorder_buf, 0, ...)
    Logger::log(INFO, ...)
  harness_b.init(cfg_b)               [same tree as harness_a.init]
  harness_a.link(&harness_b)          [LocalSimHarness.cpp:78]
  harness_a.send_message(send_env)    [LocalSimHarness.cpp:109]
    timestamp_now_us()                               — Timestamp.cpp
    m_impairment.process_outbound(send_env, now_us)  — ImpairmentEngine.cpp:151
      queue_to_delay_buf(send_env, now_us)            — ImpairmentEngine.cpp:83
        envelope_copy(m_delay_buf[0].env, send_env)
    m_impairment.collect_deliverable(now_us, ..., 32) — ImpairmentEngine.cpp:216
      envelope_copy(out_buf[0], m_delay_buf[0].env)
    m_peer->inject(delayed_envelopes[0])             — LocalSimHarness.cpp:91 [harness_b]
      m_recv_queue.push(delayed_envelopes[0])         — RingBuffer: slot 0
    m_peer->inject(send_env)                         — LocalSimHarness.cpp:91 [harness_b]
      m_recv_queue.push(send_env)                     — RingBuffer: slot 1
  harness_b.receive_message(recv_env, 100U)  [LocalSimHarness.cpp:147]
    m_recv_queue.pop(recv_env)                        — RingBuffer: pops slot 0
  harness_a.close()  [LocalSimHarness.cpp:192]
  harness_b.close()  [LocalSimHarness.cpp:192]
```

---

## 5. Key Components Involved

| Component | File | Role in this flow |
|---|---|---|
| `LocalSimHarness` | `src/platform/LocalSimHarness.cpp/.hpp` | In-process `TransportInterface`. Owns `m_recv_queue` (RingBuffer) and `m_impairment` (ImpairmentEngine). Routes send through impairment engine and injects into linked peer. |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.cpp/.hpp` | Applies configured faults. With `enabled=false`: all messages pass through delay buffer at `release_us=now_us` and are immediately collectible. |
| `RingBuffer` | `src/core/RingBuffer.hpp` | SPSC lock-free FIFO backed by `std::atomic<uint32_t>` head/tail with acquire/release ordering. Capacity `MSG_RING_CAPACITY=64`. |
| `MessageEnvelope` | `src/core/MessageEnvelope.hpp` | Standard message container. Passed by value through `envelope_copy()` at each stage. |
| `PrngEngine` | `src/platform/PrngEngine.hpp` | xorshift64 PRNG. Seeded with 42ULL during `init()`. Not consumed in this test because impairments are disabled. |
| `timestamp_now_us()` | `src/core/Timestamp.cpp` | Returns current monotonic time in microseconds via POSIX `clock_gettime`. |

---

## 6. Branching Logic / Decision Points

| # | Location | Condition | Path Taken | Effect |
|---|---|---|---|---|
| 1 | `LocalSimHarness::init()` line 59 | `config.num_channels > 0U` | `0 > 0` → false | `imp_cfg.enabled` stays `false`; impairment disabled |
| 2 | `process_outbound()` line 159 | `!m_cfg.enabled` | `!false` → true | Disabled path; PRNG not consumed; direct queue to delay buffer |
| 3 | Disabled path line 160 | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | `0 >= 32` → false | `ERR_FULL` not returned; continues to `queue_to_delay_buf()` |
| 4 | `queue_to_delay_buf()` line 91 | `!m_delay_buf[i].active` | `i=0`: true | Slot 0 chosen; `release_us = now_us` → immediately deliverable |
| 5 | `collect_deliverable()` line 229 | `active && release_us <= now_us` | `true && equal` → true | Slot 0 collected; `out_count=1` |
| 6 | `send_message()` line 118 | `res == Result::ERR_IO` | `res==OK` → false | Message not dropped; continues to inject loop |
| 7 | `receive_message()` line 153 | `result_ok(res)` after first `pop()` | Queue non-empty → true | Returns `Result::OK` immediately; polling loop never entered |

---

## 7. Concurrency / Threading Behavior

The round-trip is entirely single-threaded. No threads are created.

`RingBuffer` uses `std::atomic<uint32_t>` for `m_head` and `m_tail` with
acquire/release memory ordering to support SPSC use. In this flow, both producer
(`inject()`) and consumer (`pop()`) execute on the same thread. The acquire/release
semantics provide correct visibility.

`nanosleep()` is called inside `receive_message()` only if the first `pop()` fails.
Because `send_message()` injects two copies before `receive_message()` is called, the
first pop succeeds immediately. `nanosleep()` is never reached.

No mutexes, condition variables, or semaphores are used anywhere on this path.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

All objects are automatic (stack) variables.

| Object | Dominant size contributor | Approximate size |
|---|---|---|
| `LocalSimHarness harness_a` — `m_impairment.m_delay_buf[32]` | 32 × `MessageEnvelope` (~4120 B) | ~132 KB |
| `LocalSimHarness harness_a` — `m_impairment.m_reorder_buf[32]` | 32 × `MessageEnvelope` | ~132 KB |
| `LocalSimHarness harness_a` — `m_recv_queue.m_buf[64]` | 64 × `MessageEnvelope` | ~263 KB |
| `harness_b` | Same structure | ~527 KB |
| Misc (configs, envelopes) | Minor | ~10 KB |
| **Total** | | **~1.1 MB** |

On POSIX platforms with 8 MB default stack this is safe; on embedded targets it would
overflow.

**Ownership and lifetime:**
- `harness_a.m_peer` holds a raw pointer to `harness_b`. Both live in the same scope.
  `close()` sets it to `nullptr` before `harness_b` goes out of scope.
- `MessageEnvelope` copies flow entirely by value through `envelope_copy()`. No pointer
  aliasing between delay buffer slot, `delayed_envelopes[]`, and `recv_env`.

**RAII:**
- `LocalSimHarness::~LocalSimHarness()` calls `close()` via qualified non-virtual call
  to avoid vtable dispatch during destruction. `close()` sets `m_peer=nullptr` and
  `m_open=false`, preventing use-after-destructor.

---

## 9. Error Handling Flow

All assertions pass and all `Result` values are `OK` on the success path.

**Error paths not triggered in this test:**

| Failure condition | Detection | Consequence |
|---|---|---|
| `config.kind != LOCAL_SIM` in `init()` | `NEVER_COMPILED_OUT_ASSERT` | abort() debug / component reset production |
| `m_peer == nullptr` in `send_message()` | `NEVER_COMPILED_OUT_ASSERT` | Same |
| Delay buffer full | `process_outbound()` returns `ERR_FULL` | `send_message()` returns `ERR_FULL` |
| `RingBuffer` full in `inject()` | `push()` returns `ERR_FULL`; `WARNING_HI` logged | `send_message()` propagates `ERR_FULL` |
| No message within `timeout_ms` | `receive_message()` returns `ERR_TIMEOUT` | Not reached; first pop succeeds |

---

## 10. External Interactions

| External call | Location | Purpose |
|---|---|---|
| `timestamp_now_us()` | `send_message()` line 116 | Reads current monotonic time via POSIX `clock_gettime`. Used as `release_us` in the delay buffer slot. |
| `Logger::log(INFO, ...)` | `init()` (×2), `link()` (×1), `close()` (×4 including destructor) | Structured INFO logs; no WARNING or FATAL on success path. |
| `memset()` | `ImpairmentEngine::init()` (×2 per harness = ×4 total) | Zeroes `m_delay_buf` and `m_reorder_buf`. CRT call. |
| `nanosleep()` | `receive_message()` polling loop | **Not called.** First `pop()` succeeds immediately. |

No sockets, files, hardware, or network operations are involved.

---

## 11. State Changes / Side Effects

| Object | State after each phase |
|---|---|
| `harness_a` after `init()` | `m_open=true`, `m_peer=nullptr`, `m_impairment.m_initialized=true`, `m_prng.m_state=42` |
| `harness_a` after `link()` | `m_peer=&harness_b` |
| `harness_a.m_impairment` after `process_outbound()` | `m_delay_buf[0]={send_env copy, release_us=now_us, active=true}`, `m_delay_count=1` |
| `harness_a.m_impairment` after `collect_deliverable()` | `m_delay_buf[0].active=false`, `m_delay_count=0` |
| `harness_b.m_recv_queue` after `inject()` ×2 | `m_buf[0]=delayed copy`, `m_buf[1]=send_env copy`, `m_tail=2`, `m_head=0` |
| `harness_b.m_recv_queue` after `receive_message()` | `m_head=1`; `m_buf[1]` unconsumed |
| After `close()` and destructor | `m_open=false`, `m_peer=nullptr` for both |

Side effects: Logger INFO entries; one unconsumed envelope in `harness_b.m_recv_queue`
slot 1 (destroyed when `harness_b` goes out of scope).

---

## 12. Sequence Diagram

```
 Caller    harness_a           ImpairmentEngine         harness_b     RingBuffer(B)
    |          |                      |                      |               |
    |--init(1)->                      |                      |               |
    |          |--m_impairment.init-->|                      |               |
    |          |<--OK----------------|                      |               |
    |<--OK-----|                      |                      |               |
    |          |                      |                      |               |
    |--init(2)--------------------------------------------->|               |
    |<--OK-----------------------------------------------|               |
    |          |                      |                      |               |
    |--link(&B)->                     |                      |               |
    |          |  m_peer = &harness_b |                      |               |
    |          |                      |                      |               |
    |--send(env)->                    |                      |               |
    |          |--process_outbound -->|                      |               |
    |          |  (enabled=false)     |                      |               |
    |          |  queue_to_delay_buf  |                      |               |
    |          |  [slot0, release=now]|                      |               |
    |          |<--OK----------------|                      |               |
    |          |--collect_deliverable->                      |               |
    |          |  [slot0 ready: now<=now]                    |               |
    |          |  out_buf[0]=env copy |                      |               |
    |          |<--count=1-----------|                      |               |
    |          |--inject(delayed[0])----------------------->|               |
    |          |                      |                      |--push(env)---->|
    |          |                      |                      |  slot 0 filled |
    |          |<--------------------------------------------OK-------------|
    |          |--inject(send_env)------------------------->|               |
    |          |                      |                      |--push(env)---->|
    |          |                      |                      |  slot 1 filled |
    |          |<--------------------------------------------OK-------------|
    |<--OK-----|                      |                      |               |
    |          |                      |                      |               |
    |--recv(B,100ms)--------------------------------------------->          |
    |                                                        |--pop(recv_env)->
    |                                                        |  slot 0 popped|
    |<--OK, recv_env=send_env copy--------------------------|               |
    |          |                      |                      |               |
    |--close(A)->                     |                      |               |
    |          | m_peer=null, m_open=F|                      |               |
    |--close(B)--------------------------------------------->|               |
    |<-----------------------------------------------------------OK----------|
```

---

## 13. Initialization vs Runtime Flow

### Initialization phase

- `LocalSimHarness` constructors: `m_peer=nullptr`, `m_open=false`.
- `LocalSimHarness::init()`: initializes `RingBuffer` (zero atomics), initializes
  `ImpairmentEngine` (seeds PRNG to 42, zeroes delay and reorder buffers), sets
  `m_open=true`.
- `LocalSimHarness::link()`: sets `m_peer` to point at the peer harness.
- All initialization completes before any send or receive operation.

### Runtime phase

- `send_message()`: `process_outbound()` → `queue_to_delay_buf()` (delay buffer slot 0
  filled) → `collect_deliverable()` (slot 0 retrieved, `out_count=1`) →
  `inject(delayed[0])` (RingBuffer slot 0) → `inject(send_env)` (RingBuffer slot 1).
- `receive_message()`: `pop()` on `m_recv_queue` — first pop succeeds immediately
  (slot 0 available).

The disabled impairment path means the ImpairmentEngine behavior is trivial: accept
the message into the delay buffer at `now_us`, then immediately return it from
`collect_deliverable()` because `release_us <= now_us`.

---

## 14. Known Risks / Observations

**Risk 1 — Double injection of the same message (disabled impairment path).**
When `enabled=false`, `send_message()` injects the envelope into the peer's
`RingBuffer` twice: once as `delayed_envelopes[0]` from `collect_deliverable()` and
once as the original envelope. The receiver's queue contains two identical copies.
Capacity: `MSG_RING_CAPACITY=64`. Any test that calls `receive_message()` twice after
a single `send_message()` with `enabled=false` will observe a spurious second delivery.

**Risk 2 — Large stack allocation.**
Each `LocalSimHarness` contains an `ImpairmentEngine` with two fixed arrays of 32
`MessageEnvelope` objects plus a `RingBuffer` of 64 envelopes. With
`MSG_MAX_PAYLOAD_BYTES=4096`, two harnesses use approximately 1.1 MB of stack.
Safe on POSIX (8 MB default stack) but would overflow embedded targets.

**Risk 3 — Duplicate destructor log.**
`close()` is called explicitly by the test, then again by the destructor. Each call
emits an INFO "Transport closed" log, producing duplicate entries per harness.

**Risk 4 — Implicit `m_peer` lifetime dependency.**
`harness_a.m_peer` points to `harness_b`. Both live in the same function. `close()`
nulls the pointer before `harness_b` is destroyed. If the declaration order of the
two harnesses were reversed, the destructor execution order would reverse — still safe
but dependent on declaration ordering.

**Risk 5 — `release_us == now_us` same-timestamp assumption.**
The disabled path sets `release_us = now_us` and immediately calls
`collect_deliverable(now_us, ...)`. The condition `release_us <= now_us` is trivially
true when both use the same snapshot. The current implementation passes `now_us` by
value; this is correct.

**Risk 6 — Unconsumed envelope in peer queue.**
After the test completes, `harness_b.m_recv_queue` contains one unconsumed envelope
at slot 1. Re-calling `init()` before reuse correctly zeroes the `RingBuffer` atomics.

---

## 15. Unknowns / Assumptions

`[ASSUMPTION]` `transport_config_default()` sets `num_channels = 0`, causing the
`if (config.num_channels > 0U)` branch in `init()` not to be taken, keeping
`imp_cfg.enabled=false`. This is consistent with the disabled impairment path observed.

`[ASSUMPTION]` `envelope_valid()` accepts envelopes with `expiry_time_us=0` and
`payload_length=5`. The validation function is in `MessageEnvelope.hpp`; no assertion
failure occurs in the test, confirming the assumption.

`[ASSUMPTION]` `RingBuffer` uses a power-of-two capacity mask for slot indexing
(`MSG_RING_CAPACITY=64 = 2^6`).

`[ASSUMPTION]` `timestamp_now_us()` returns microsecond-resolution monotonic time
via POSIX clock. Its exact value does not affect test correctness; only that
`collect_deliverable()` receives the same snapshot value.

`[ASSUMPTION]` The `ImpairmentEngine` constructor pre-seeds `m_prng` to `1ULL` before
`init()` re-seeds it to `42ULL`. The constructor's preliminary seed is irrelevant;
`init()` overwrites it unconditionally.
