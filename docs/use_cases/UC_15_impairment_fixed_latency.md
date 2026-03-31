# UC_15 — Fixed latency: message held in delay buffer until release_us elapses

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.1, REQ-5.2.1, REQ-5.2.2, REQ-5.3.1, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** A caller invokes `TcpBackend::send_message()` (or any backend that drives the impairment engine) with a valid `MessageEnvelope`. The `ImpairmentConfig` supplied at `init()` time has `enabled = true`, `fixed_latency_ms > 0`, and `loss_probability = 0.0` (so the message is not dropped by loss).

**Goal:** The message is withheld from the network for exactly `fixed_latency_ms` milliseconds by buffering it in the internal `m_delay_buf` array. Only after `collect_deliverable()` is called with a `now_us` timestamp that has advanced past `release_us` does the message enter the wire-send path.

**Success outcome:** `process_outbound()` returns `Result::OK`; the envelope occupies one slot in `m_delay_buf` with `active = true` and `release_us = now_us + fixed_latency_ms * 1000`; a subsequent call to `flush_delayed_to_clients()` at `now_us >= release_us` extracts the envelope and hands it to `send_to_all_clients()`.

**Error outcomes:**
- `ERR_FULL` — `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` (32 slots full) before queuing.
- `ERR_IO` — partition is active or loss roll fires (not applicable in this UC; loss probability is 0).

---

## 2. Entry Points

Primary entry (called by TcpBackend):

```
// src/platform/ImpairmentEngine.hpp / ImpairmentEngine.cpp
Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env,
                                          uint64_t now_us);
```

Called from:

```
// src/platform/TcpBackend.cpp
Result TcpBackend::send_message(const MessageEnvelope& envelope);
```

The collection half of the flow (releasing the buffered message) is triggered from:

```
void TcpBackend::flush_delayed_to_clients(uint64_t now_us);
```

which calls:

```
uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us,
                                                MessageEnvelope* out_buf,
                                                uint32_t buf_cap);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase A — Buffering the message**

1. `TcpBackend::send_message(envelope)` is called by the application.
2. Preconditions checked: `NEVER_COMPILED_OUT_ASSERT(m_open)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` converts the envelope to big-endian bytes into the shared `m_wire_buf`. Returns `OK` or the call aborts with a log warning and returns the error.
4. `timestamp_now_us()` is called to obtain the current wall-clock time (`now_us`).
5. `m_impairment.process_outbound(envelope, now_us)` is called.
6. Inside `process_outbound()`:
   a. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
   b. `m_cfg.enabled` is `true` — impairments are active; skip the pass-through branch.
   c. `is_partition_active(now_us)` is called. Returns `false` (no partition). Message is not dropped.
   d. `check_loss()` is called. `m_cfg.loss_probability <= 0.0` so the early-exit guard fires and returns `false` immediately without consuming a PRNG call. Message is not dropped.
   e. Fixed delay is computed: `base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL`.
   f. Jitter check: `m_cfg.jitter_mean_ms == 0` (no jitter in this UC), so `jitter_us = 0`.
   g. `release_us = now_us + base_delay_us + 0`.
   h. Capacity guard: `m_delay_count < IMPAIR_DELAY_BUF_SIZE` — space is available.
   i. `queue_to_delay_buf(in_env, release_us)` is called.
7. Inside `queue_to_delay_buf()`:
   a. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE)`.
   b. Linear scan of `m_delay_buf[0..IMPAIR_DELAY_BUF_SIZE-1]` for the first slot with `active == false`.
   c. First free slot found at index `i`: `envelope_copy(m_delay_buf[i].env, env)`, `m_delay_buf[i].release_us = release_us`, `m_delay_buf[i].active = true`, `++m_delay_count`.
   d. `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
   e. Returns `Result::OK`.
8. Back in `process_outbound()`: duplication check — `m_cfg.duplication_probability == 0.0`, so `apply_duplication()` is not called.
9. `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`. Returns `Result::OK`.
10. Back in `TcpBackend::send_message()`: `res == OK`, not `ERR_IO`, so the message was accepted.
11. `m_client_count == 0` check — assume at least one client is connected; continue.
12. `flush_delayed_to_clients(now_us)` is called immediately.

**Phase B — Immediate flush attempt (message not yet deliverable)**

13. Inside `flush_delayed_to_clients(now_us)`:
    a. `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` declared on the stack (fixed-size).
    b. `m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)` is called.
14. Inside `collect_deliverable(now_us, ...)`:
    a. Linear scan of `m_delay_buf[0..IMPAIR_DELAY_BUF_SIZE-1]`.
    b. The newly queued slot has `active == true` but `release_us > now_us` (latency has not elapsed). Condition `m_delay_buf[i].release_us <= now_us` is `false`.
    c. No slots are collected. Returns `0`.
15. `count == 0`; the loop body in `flush_delayed_to_clients()` does not execute. No bytes are sent.
16. `send_message()` returns `Result::OK` to the caller — the message has been accepted but not yet transmitted.

**Phase C — Release when latency elapses**

17. Later, `send_message()` is called again (or a periodic flush is triggered), obtaining a new `now_us` where `now_us >= release_us`.
18. `flush_delayed_to_clients(now_us)` is called.
19. `collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)` scans again:
    a. The slot now satisfies `release_us <= now_us`.
    b. `envelope_copy(out_buf[out_count], m_delay_buf[i].env)`, `++out_count`.
    c. `m_delay_buf[i].active = false`, `--m_delay_count`.
    d. Returns `1`.
20. Back in `flush_delayed_to_clients()`, `count == 1`. For slot `0`:
    a. `Serializer::serialize(delayed[0], m_wire_buf, SOCKET_RECV_BUF_BYTES, delayed_len)` — re-serializes the envelope.
    b. `send_to_all_clients(m_wire_buf, delayed_len)` is called.
21. Inside `send_to_all_clients()`:
    a. Iterates over `m_client_fds[0..MAX_TCP_CONNECTIONS-1]`.
    b. For each fd with `m_client_fds[i] >= 0`, calls `m_sock_ops->send_frame(fd, buf, len, send_timeout_ms)`.
    c. On `send_frame` failure, logs `WARNING_LO` and continues to the next client.
22. Message is now on the wire. The fixed latency delay has been applied.

---

## 4. Call Tree

```
TcpBackend::send_message()
 ├── Serializer::serialize()                       [wire format; result checked]
 ├── timestamp_now_us()                            [wall clock]
 ├── ImpairmentEngine::process_outbound()
 │    ├── ImpairmentEngine::is_partition_active()  [returns false]
 │    ├── ImpairmentEngine::check_loss()           [returns false; loss_prob == 0]
 │    └── ImpairmentEngine::queue_to_delay_buf()   [stores envelope + release_us]
 │         └── envelope_copy()                     [memcpy into delay slot]
 └── TcpBackend::flush_delayed_to_clients()
      └── ImpairmentEngine::collect_deliverable()  [returns 0 (too early) or N]
           └── (if count > 0)
                ├── Serializer::serialize()        [re-serialize delayed envelope]
                └── TcpBackend::send_to_all_clients()
                     └── ISocketOps::send_frame()  [tcp_send_frame() via vtable]
                          ├── socket_send_all()
                          │    └── poll() + send() [POSIX]
                          └── (4-byte header + payload)
```

---

## 5. Key Components Involved

- **`ImpairmentEngine`** (`src/platform/ImpairmentEngine.hpp/.cpp`): Owns `m_delay_buf[32]`, `m_delay_count`, `m_prng`, and `m_cfg`. Executes the latency logic inside `process_outbound()`. Central to this flow.

- **`ImpairmentEngine::queue_to_delay_buf()`** (private): Finds a free slot in `m_delay_buf`, copies the envelope via `envelope_copy()`, and sets `release_us`. The only write path into the delay buffer.

- **`ImpairmentEngine::collect_deliverable()`**: Scans `m_delay_buf` for entries where `release_us <= now_us`, copies them to the output buffer, and marks those slots inactive. Decrements `m_delay_count` for each.

- **`ImpairmentEngine::check_loss()`** (private): Consults `m_cfg.loss_probability`. When probability is `<= 0.0`, returns `false` immediately without consuming a PRNG call. In this UC loss is 0 so PRNG is not advanced.

- **`ImpairmentEngine::is_partition_active()`**: State machine maintaining partition timing. Always called before loss check. Returns `false` here (no partition configured).

- **`PrngEngine`** (`src/platform/PrngEngine.hpp`): xorshift64 PRNG. Not invoked in the fixed-latency-only path because `loss_probability == 0` short-circuits before any `next_double()` call and `jitter_mean_ms == 0` skips `next_range()`.

- **`TcpBackend`** (`src/platform/TcpBackend.hpp/.cpp`): Owns `m_impairment` by value and `m_wire_buf[8192]`. Calls `process_outbound()` and `flush_delayed_to_clients()` in sequence on every `send_message()`.

- **`Serializer`** (`src/core/Serializer.hpp`): Converts `MessageEnvelope` to big-endian wire bytes. Called twice per delayed message: once in `send_message()` (bytes go to `m_wire_buf` but are not transmitted yet) and again in `flush_delayed_to_clients()` when the slot is released.

- **`ISocketOps` / `SocketOpsImpl`** (`src/platform/ISocketOps.hpp`): Virtual interface wrapping `tcp_send_frame()`. Allows mock injection in tests. `send_frame()` delegates to `socket_send_all()` which handles partial writes.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!m_cfg.enabled` | Pass-through: `queue_to_delay_buf(in_env, now_us)` — release_us equals now_us | Continue to partition, loss, and latency checks |
| `is_partition_active(now_us)` | Log WARNING_LO "partition active"; return `ERR_IO` (message dropped) | Continue |
| `check_loss()` returns `true` | Log WARNING_LO "loss probability"; return `ERR_IO` (message dropped) | Compute `release_us` and queue |
| `m_cfg.jitter_mean_ms > 0` | `jitter_us = prng.next_range(0, jitter_variance_ms) * 1000` | `jitter_us = 0` |
| `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | Log WARNING_HI "delay buffer full"; return `ERR_FULL` | Call `queue_to_delay_buf()` |
| `!m_delay_buf[i].active` in scan | Use this slot; copy envelope; `active = true`; `++m_delay_count`; return OK | Skip slot, continue scan |
| Full scan with no free slot | `NEVER_COMPILED_OUT_ASSERT(false)` (logic error — unreachable if capacity guard held) | (unreachable) |
| `m_cfg.duplication_probability > 0.0` | Call `apply_duplication()` — may queue a second copy | Skip |
| `m_delay_buf[i].active && release_us <= now_us` in `collect_deliverable` | Copy to output; `active = false`; `--m_delay_count` | Skip slot |
| `result_ok(Serializer::serialize())` in `flush_delayed_to_clients` | Call `send_to_all_clients()` | `continue` — delayed message silently dropped |
| `m_sock_ops->send_frame()` returns `false` | Log WARNING_LO; continue to next client | No action needed |

---

## 7. Concurrency / Threading Behavior

`ImpairmentEngine` contains no synchronization primitives. It is designed for single-threaded use per instance.

- All calls to `process_outbound()`, `collect_deliverable()`, and `is_partition_active()` run on whichever thread calls `TcpBackend::send_message()`. No locks are held anywhere in this call chain.
- `TcpBackend::m_impairment` is a by-value member object. It is not shared between `send_message()` and `receive_message()` paths in any concurrent sense; each call enters the engine serially.
- No `std::atomic` variables are involved. `m_delay_count` and `m_delay_buf` are plain `uint32_t` and struct arrays.
- `timestamp_now_us()` returns a `uint64_t` by value — no shared mutable state.

[ASSUMPTION: the application uses TcpBackend from a single thread, or applies external locking before calling `send_message()`.]

---

## 8. Memory & Ownership Semantics

**Stack allocations in this flow:**

| Variable | Declared in | Approximate size |
|----------|-------------|-----------------|
| `delayed[IMPAIR_DELAY_BUF_SIZE]` | `flush_delayed_to_clients()` | 32 × sizeof(MessageEnvelope) ≈ 132 KB [ASSUMPTION] |
| `header[4]` | `tcp_send_frame()` (inside `send_frame`) | 4 bytes |

**Fixed member arrays (heap-free, allocated at object construction):**

| Member | Owner class | Capacity constant |
|--------|-------------|-------------------|
| `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` | `ImpairmentEngine` | `IMPAIR_DELAY_BUF_SIZE = 32` |
| `m_reorder_buf[IMPAIR_DELAY_BUF_SIZE]` | `ImpairmentEngine` | `IMPAIR_DELAY_BUF_SIZE = 32` |
| `m_client_fds[MAX_TCP_CONNECTIONS]` | `TcpBackend` | `MAX_TCP_CONNECTIONS = 8` |
| `m_wire_buf[SOCKET_RECV_BUF_BYTES]` | `TcpBackend` | `SOCKET_RECV_BUF_BYTES = 8192` |

**Power of 10 Rule 3 confirmation:** No dynamic allocation (`new`/`malloc`) occurs anywhere in this flow. All buffers are either member arrays allocated at object construction time or fixed-size stack locals. `envelope_copy()` uses `memcpy` into pre-allocated `DelayEntry.env` members.

**Ownership:** `TcpBackend` owns `m_impairment` by value (not pointer). `m_sock_ops` in `TcpBackend` is a non-owning raw pointer set at construction — the pointed-to `ISocketOps` implementation must outlive the `TcpBackend` instance.

---

## 9. Error Handling Flow

| Error | Trigger | State after | Caller action |
|-------|---------|-------------|---------------|
| `ERR_IO` from `process_outbound()` | Partition active or loss roll fired | Delay buffer unchanged; `m_delay_count` unchanged | `send_message()` treats `ERR_IO` as a silent drop; returns `Result::OK` to caller |
| `ERR_FULL` from `process_outbound()` | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` before call to `queue_to_delay_buf` | Delay buffer full; message not queued | `send_message()` propagates `ERR_FULL` to caller |
| `NEVER_COMPILED_OUT_ASSERT(false)` in `queue_to_delay_buf()` | No free slot found despite capacity guard (logic error) | In debug: `abort()`. In production: `IResetHandler::on_fatal_assert()` | Component reset |
| Serialize failure in `send_message()` | `Serializer::serialize()` returns non-OK | `m_wire_buf` partially written; message not queued in engine | `send_message()` logs WARNING_LO; returns error code |
| Serialize failure in `flush_delayed_to_clients()` | Second `Serializer::serialize()` returns non-OK | Delayed slot already freed from `m_delay_buf` | Silent `continue`; message is lost |
| `send_frame()` failure per client | POSIX `send()` or `poll()` error | That client's fd remains open; no retry | `send_to_all_clients()` logs WARNING_LO; continues to remaining clients |

---

## 10. External Interactions

During the **buffering phase** (`process_outbound()` → `queue_to_delay_buf()`): no POSIX calls are made. All work is in-memory.

During the **release phase** (`flush_delayed_to_clients()` → `send_to_all_clients()` → `ISocketOps::send_frame()` → `tcp_send_frame()`):

- `poll(pfds, 1, timeout_ms)` — called inside `socket_send_all()` before each `send()` to confirm the fd is writable. Uses the per-channel `send_timeout_ms`.
- `send(fd, &buf[sent], remaining, 0)` — POSIX TCP send on each connected client fd in `m_client_fds[]`.

`timestamp_now_us()` is called by `TcpBackend::send_message()` via `src/core/Timestamp.hpp`. [ASSUMPTION: wraps `clock_gettime(CLOCK_MONOTONIC, ...)` or equivalent POSIX API.]

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | `m_delay_buf[i].active` | `false` (free slot) | `true` (occupied) |
| `ImpairmentEngine` | `m_delay_buf[i].env` | zero-initialized or stale | Copy of `in_env` via `memcpy` |
| `ImpairmentEngine` | `m_delay_buf[i].release_us` | `0` or stale | `now_us + fixed_latency_ms * 1000` |
| `ImpairmentEngine` | `m_delay_count` | N | N + 1 (Phase A) |
| `ImpairmentEngine` | `m_delay_buf[i].active` | `true` | `false` (Phase C — slot freed) |
| `ImpairmentEngine` | `m_delay_count` | N + 1 | N (Phase C) |
| `TcpBackend` | `m_wire_buf` | previous contents | Serialized form of released envelope (Phase C) |
| Network / OS kernel | connected TCP fd | no data pending | Frame bytes in kernel send buffer, in-flight to connected clients |

`m_partition_active`, `m_next_partition_event_us`, and `m_prng.m_state` are unchanged when `loss_probability == 0` and `jitter_mean_ms == 0` (no PRNG calls occur in this path).

---

## 12. Sequence Diagram

```
Caller            TcpBackend           ImpairmentEngine     ISocketOps(impl)
  |                    |                      |                    |
  |--send_message()--->|                      |                    |
  |                    |--serialize()-------->|                    |
  |                    |  (via Serializer)    |                    |
  |                    |--timestamp_now_us()  |                    |
  |                    |--process_outbound()->|                    |
  |                    |                      |--is_partition?     |
  |                    |                      |  (false)           |
  |                    |                      |--check_loss()      |
  |                    |                      |  (false; prob=0)   |
  |                    |                      |--compute           |
  |                    |                      |  release_us =      |
  |                    |                      |  now_us + lat_ms   |
  |                    |                      |  * 1000            |
  |                    |                      |--queue_to_delay_   |
  |                    |                      |  buf(env, rel_us)  |
  |                    |                      |  slot.active=true  |
  |                    |                      |  m_delay_count++   |
  |                    |<-----OK--------------|                    |
  |                    |--flush_delayed_      |                    |
  |                    |  to_clients(now_us)  |                    |
  |                    |  collect_deliverable |                    |
  |                    |  (now_us) → 0        |                    |
  |                    |  (release_us > now)  |                    |
  |<---OK--------------|                      |                    |
  |                                           |                    |
  | ...time passes (fixed_latency_ms elapses) |                    |
  |                                           |                    |
  |--send_message()--->|  (next call or flush)|                    |
  |  (or periodic)     |--flush_delayed_      |                    |
  |                    |  to_clients(now_us2) |                    |
  |                    |  collect_deliverable |                    |
  |                    |  (now_us2) → 1       |                    |
  |                    |  slot.active=false   |                    |
  |                    |  m_delay_count--     |                    |
  |                    |--serialize(delayed[0])|                   |
  |                    |--send_to_all_clients()|                   |
  |                    |                      |   send_frame()---->|
  |                    |                      |   poll()+send()    |
  |<---OK--------------|                      |                    |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (one-time, before any messages):**

- `ImpairmentEngine::ImpairmentEngine()`: zeros `m_delay_buf` via `memset`; sets `m_delay_count = 0`, `m_reorder_count = 0`; seeds PRNG with `1ULL` (temporary seed); sets `m_initialized = false`.
- `ImpairmentEngine::init(cfg)`: stores `cfg` into `m_cfg`; re-seeds PRNG with `cfg.prng_seed` (or `42` if zero); re-zeros `m_delay_buf` and `m_reorder_buf`; resets partition state to `m_partition_active = false`, `m_next_partition_event_us = 0`; sets `m_initialized = true`.
- `TcpBackend::init(config)`: calls `m_recv_queue.init()` and `m_impairment.init(imp_cfg)` before attempting `bind_and_listen()` or `connect_to_server()`.

**Steady-state runtime:**

- Every `send_message()` call reads `now_us`, invokes `process_outbound()` (which may queue a new delay-buffer slot), and immediately calls `flush_delayed_to_clients(now_us)` to release any slots whose `release_us` has elapsed.
- There is no background thread or timer driving the release. The flush is triggered synchronously on every outbound send. If sends are infrequent relative to the configured latency, the message remains buffered until the next `send_message()` call provides a `now_us` that crosses `release_us`.

---

## 14. Known Risks / Observations

- **`IMPAIR_DELAY_BUF_SIZE = 32` hard ceiling.** If 32 delayed messages accumulate before any are released (i.e., the send rate exceeds the release rate), the 33rd `send_message()` call returns `ERR_FULL` and the message is discarded. No overflow is possible because the capacity guard precedes `queue_to_delay_buf()`, but callers must handle backpressure.

- **No dedicated timer thread.** Delayed messages are only released during `flush_delayed_to_clients()`, which is called from `send_message()`. If the application sends infrequently relative to the configured latency, released messages accumulate in the delay buffer for longer than intended.

- **`m_wire_buf` is shared.** The initial `Serializer::serialize()` call in `send_message()` writes to `m_wire_buf`, but in the fixed-latency path those bytes are never transmitted — the buffer is overwritten again in `flush_delayed_to_clients()` at release time. This double serialization is safe but wasteful, and the first serialization result being silently discarded is non-obvious.

- **Double serialization.** Every delayed message is serialized twice: once during `send_message()` (discarded) and again in `flush_delayed_to_clients()` (transmitted). This increases CPU load proportionally to the number of delayed messages.

- **Large stack frame in `flush_delayed_to_clients()`.** `delayed[IMPAIR_DELAY_BUF_SIZE]` is approximately 132 KB on the stack, which may be significant on constrained platforms.

- **`is_partition_active()` advances its state machine on every `process_outbound()` call.** Even when only fixed latency is needed, the partition state machine is polled. If `partition_gap_ms` is very small, unexpected transitions could occur.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `timestamp_now_us()` wraps `clock_gettime(CLOCK_MONOTONIC, ...)`. The actual implementation resides in `src/core/Timestamp.hpp` (not read for this document).
- [ASSUMPTION] `sizeof(MessageEnvelope)` is approximately 4120 bytes (header fields + 4096-byte payload array), making `delayed[32]` approximately 132 KB on the stack.
- [ASSUMPTION] The application calls `send_message()` at sufficient frequency that messages are released from the delay buffer within a bounded time window after their `release_us` has elapsed.
- [ASSUMPTION] The initial `Serializer::serialize()` call in `send_message()` serves as an envelope validation step. Its byte output into `m_wire_buf` is not used in the fixed-latency path.
- [ASSUMPTION] `PrngEngine::next_double()` is not called in the pure fixed-latency path. If any caller also enables non-zero `loss_probability`, PRNG state will be consumed by `check_loss()` before computing `release_us`.
- [ASSUMPTION] Only the first channel's `ImpairmentConfig` (`config.channels[0U].impairment`) is used by `TcpBackend`. A single `ImpairmentEngine` instance is applied to all traffic regardless of channel.
