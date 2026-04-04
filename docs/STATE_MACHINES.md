# State Machine Specifications

**Project:** messageEngine
**Standard:** NASA-STD-8719.13C §4 (safety-critical state machine documentation);
             NASA-STD-8739.8A §6 (software assurance — formal specification)
**Policy reference:** CLAUDE.md §15
**Software classification:** Class C per NPR 7150.2D (Appendix D) — this library has
no direct human-safety implication in isolation; it is infrastructure software whose
safety properties are enforced by the application that embeds it. Reclassification
to Class A or B triggers a formal model-checking obligation (TLA+, SPIN, or
equivalent) for all state machines documented here.

---

## §1 AckTracker State Machine

**Source:** src/core/AckTracker.hpp / AckTracker.cpp
**Purpose:** Tracks outstanding messages awaiting ACK; detects timeouts.
**Hazards mitigated:** HAZ-002 (retry storm), HAZ-006 (resource exhaustion)

### States

| State | Value | Meaning |
|-------|-------|---------|
| `FREE` | 0 | Slot is available for use |
| `PENDING` | 1 | Message sent; ACK not yet received; deadline not yet passed |
| `ACKED` | 2 | ACK received; slot will be freed on next sweep |

### Transition Table

| Current State | Event | Guard | Next State | Action |
|---------------|-------|-------|------------|--------|
| `FREE` | `track(env, deadline)` | `m_count < ACK_TRACKER_CAPACITY` | `PENDING` | Copy env; set deadline; increment m_count |
| `FREE` | `track(env, deadline)` | `m_count == ACK_TRACKER_CAPACITY` | `FREE` | Return `ERR_FULL`; logging deferred to caller (DeliveryEngine logs `WARNING_HI`) |
| `PENDING` | `on_ack(src, msg_id)` | src and msg_id match slot | `ACKED` | — |
| `PENDING` | `on_ack(src, msg_id)` | src or msg_id does not match | `PENDING` | Returns `ERR_INVALID`; slot remains PENDING until `sweep_expired()` fires. |
| `PENDING` | `sweep_expired()` | `now_us >= deadline_us` | `FREE` | Copy to expired_buf if space; decrement m_count |
| `PENDING` | `sweep_expired()` | `now_us < deadline_us` | `PENDING` | No action |
| `PENDING` | `cancel(src, msg_id)` | src and msg_id match slot | `FREE` | Decrement m_count; no stat bump; used for send-failure rollback only |
| `PENDING` | `cancel(src, msg_id)` | src or msg_id does not match | `PENDING` | Returns `ERR_INVALID`; slot unchanged |
| `ACKED` | `sweep_expired()` | — | `FREE` | Decrement m_count |

### Invariants

- `0 ≤ m_count ≤ ACK_TRACKER_CAPACITY` at all times.
- A slot in `ACKED` state is freed on the very next `sweep_expired()` call.
- A slot in `PENDING` state is freed by: (a) `sweep_expired()` when the deadline passes — copied to `expired_buf` if space available; or (b) `cancel()` on send-failure rollback — freed directly to `FREE` with no stat bump and no copy to `expired_buf`.
- `on_ack()` never decrements `m_count` — only `sweep_expired()` does.

> **Note:** `cancel()` is a rollback-only path, callable exclusively from `DeliveryEngine::send()` on transport failure before any wire I/O. It must not be called after a message has been delivered to the wire.

> **Defect fixed (DEF-003-1):** `on_ack(src, msg_id)` matches on `slot.env.source_id == src`. Previously, `DeliveryEngine::receive()` passed `env.source_id` (the remote ACK sender's ID) as `src`, which never matched the locally-assigned `source_id` stored at `track()` time. Fixed by passing `env.destination_id` (the local node ID — the original message sender) instead. The PENDING→ACKED transition now fires correctly in normal two-node deployments.

### State Diagram

```
         track() [capacity ok]
FREE ──────────────────────────► PENDING
 ▲                                  │ │ │
 │   sweep_expired() [expired]       │ │ │ on_ack()
 │◄──────────────────────────────────┘ │ │
 │                                     │ ▼
 │   cancel() [rollback]               │ ACKED
 │◄────────────────────────────────────┘
 │         sweep_expired()
 └◄──────────────────────────────────────
```

---

## §2 RetryManager State Machine

**Source:** src/core/RetryManager.hpp / RetryManager.cpp
**Purpose:** Schedules messages for exponential-backoff retry; enforces max_retries.
**Hazards mitigated:** HAZ-002 (retry storm via backoff + max_retries)

### States

Each slot in `m_slots[]` is modelled as a two-valued active flag plus counters. The
logical states are:

| Logical State | `active` | Condition | Meaning |
|---------------|----------|-----------|---------|
| `INACTIVE` | false | — | Slot is free |
| `WAITING` | true | `next_retry_us > now_us` | Retry scheduled; not yet due |
| `DUE` | true | `next_retry_us ≤ now_us` AND `retry_count < max_retries` AND `expiry_us > now_us` | Ready to retransmit |
| `EXHAUSTED` | true | `retry_count ≥ max_retries` | No more retries; will be reaped |
| `EXPIRED` | true | `now_us ≥ expiry_us` | Message lifetime elapsed; will be reaped |

### Transition Table

| Current State | Event | Guard | Next State | Action |
|---------------|-------|-------|------------|--------|
| `INACTIVE` | `schedule(env, max, backoff, now)` | free slot exists | `WAITING` | Copy env; set next_retry_us = now_us; set expiry_us; increment m_count |
| `INACTIVE` | `schedule(env, max, backoff, now)` | no free slot | `INACTIVE` | Return `ERR_FULL`; log `WARNING_LO` |
| `WAITING` | `collect_due(now, buf)` | `next_retry_us > now_us` | `WAITING` | No action |
| `WAITING` | `collect_due(now, buf)` | `now_us ≥ expiry_us` | `INACTIVE` | Decrement m_count; log `WARNING_LO` (expired) |
| `WAITING` | `collect_due(now, buf)` | `retry_count ≥ max_retries` | `INACTIVE` | Decrement m_count; log `WARNING_HI` (exhausted) |
| `WAITING` | `on_ack(src, msg_id)` | src and msg_id match | `INACTIVE` | Decrement m_count; log INFO |
| `WAITING` | `on_ack(src, msg_id)` | src or msg_id does not match | `WAITING` | Returns `ERR_INVALID`; no state change. |
| `DUE` | `collect_due(now, buf)` | buf has space | `WAITING` | Copy to buf; increment retry_count; advance backoff; set next_retry_us |
| `DUE` | `collect_due(now, buf)` | buf full | `DUE` | Not collected this sweep; will retry on next call |

### Invariants

- `0 ≤ m_count ≤ ACK_TRACKER_CAPACITY` at all times.
- `backoff_ms` is monotonically non-decreasing per slot, capped at 60 000 ms.
- `retry_count` is monotonically non-decreasing per slot.
- A slot transitions to `INACTIVE` only through: ACK received, exhausted, or expired.
- The `schedule()` call sets `next_retry_us = now_us` so the first retry fires immediately.

> **Defect fixed (DEF-003-1):** The source_id mismatch described for AckTracker §1 applied equally to RetryManager. `DeliveryEngine::receive()` now passes `env.destination_id` (local node ID) to `RetryManager::on_ack()`, which correctly matches the stored `slot.env.source_id` (also the local node ID set at `schedule()` time). ACK receipt now cancels retry slots via `on_ack()` in both AckTracker and RetryManager as intended.

---

## §3 ImpairmentEngine State Machine

**Source:** src/platform/ImpairmentEngine.hpp / ImpairmentEngine.cpp
**Purpose:** Applies configurable network impairments (loss, latency, jitter,
duplication, reordering, partition) to outbound and inbound messages.
**Hazards mitigated:** HAZ-002 (retry storm via jitter), HAZ-003 (duplicate injection),
HAZ-004 (latency-induced stale messages), HAZ-007 (partition masking)

### States

The ImpairmentEngine has two orthogonal state dimensions:

**Dimension 1 — Initialisation:**

| State | Condition | Meaning |
|-------|-----------|---------|
| `UNINIT` | `!m_initialized` | `init()` not yet called; all operations assert-fail |
| `READY` | `m_initialized` | Ready to process messages |

**Dimension 2 — Partition (within `READY`):**

| State | Fields | Meaning |
|-------|--------|---------|
| `NO_PARTITION` | `m_partition_active == false` | Link is up; messages pass through impairment pipeline |
| `PARTITION_ACTIVE` | `m_partition_active == true` | Simulated link outage; all outbound messages dropped with `ERR_IO` |

Transitions are driven by `m_next_partition_event_us` (uint64_t) and the `now_us` argument passed to each `process_outbound()` call. The initial state is always `NO_PARTITION`; the first active partition begins after `partition_gap_ms` elapses from the first call.

### Transition Table — Initialisation Dimension

| Current State | Event | Next State | Action |
|---------------|-------|------------|--------|
| `UNINIT` | `init(cfg)` | `READY` | Seed PRNG; zero delay buffer; set m_initialized |
| `READY` | Any operation | `READY` | Normal processing |

### Transition Table — Partition Dimension (within READY)

`is_partition_active(now_us)` is the sole transition function. Its internal logic:

| Condition | Action | Returns |
|-----------|--------|---------|
| `!m_cfg.partition_enabled` | No state change | `false` |
| `m_next_partition_event_us == 0` (first call) | `m_partition_active = false`; `m_next_partition_event_us = now_us + partition_gap_ms × 1000` | `false` |
| `!m_partition_active && now_us >= m_next_partition_event_us` | `m_partition_active = true`; `m_partition_start_us = now_us`; `m_next_partition_event_us = now_us + partition_duration_ms × 1000`; log `WARNING_LO` | `true` |
| `m_partition_active && now_us >= m_next_partition_event_us` | `m_partition_active = false`; `m_next_partition_event_us = now_us + partition_gap_ms × 1000`; log `WARNING_LO` | `false` |
| Otherwise | No state change | `m_partition_active` |

Effect on `process_outbound()`:

| `is_partition_active()` | `process_outbound()` action |
|-------------------------|----------------------------|
| `true` | Drop message; log `WARNING_LO`; return `ERR_IO` |
| `false` | Apply loss/jitter/duplication; queue to delay buffer |

> **Known edge case:** If `partition_gap_ms == 0`, the first call to `is_partition_active()` sets `m_next_partition_event_us = now_us`. On the very next call (same or greater `now_us`), the `!m_partition_active && now_us >= m_next_partition_event_us` guard fires, activating a partition immediately and permanently (the gap phase has zero duration). This causes a permanent partition — all messages are dropped.

### Delay Buffer Sub-State

| Sub-state | Condition | Meaning |
|-----------|-----------|---------|
| `SLOT_FREE` | `!m_delay_buf[i].active` | Delay slot available |
| `SLOT_HELD` | `m_delay_buf[i].active AND release_us > now_us` | Message in buffer; release time not reached |
| `SLOT_READY` | `m_delay_buf[i].active AND release_us ≤ now_us` | Message ready for delivery |

`collect_deliverable(now_us, buf, cap)` transitions `SLOT_READY → SLOT_FREE` for all
ready slots, copying envelopes to the caller's buffer.

### Invariants

- `m_initialized` is set exactly once, in `init()`.
- `m_delay_count` equals the number of `active == true` slots in `m_delay_buf`.
- `0 ≤ m_delay_count ≤ IMPAIR_DELAY_BUF_SIZE` at all times.
- `process_outbound()` checks `is_partition_active()` as its first action — no
  message is queued to the delay buffer during a partition.
- If `m_cfg.enabled == false`, messages are queued to the delay buffer with
  `release_us = now_us` (immediate pass-through), preserving the pipeline interface.

---

## §4 Software Classification and Formal Methods Applicability

**Classification:** Class C (NPR 7150.2D Appendix D)

Rationale: messageEngine is infrastructure software — a networking library. It does
not directly command actuators, life-support systems, or propulsion. Safety
properties it enforces (deduplication, expiry, retry bounds) are preconditions for
application-level safety, not the primary safety barrier. The application embedding
this library is responsible for its own Class A/B-level assurance if required.

**Formal Methods Trigger:**

If this library is reclassified to Class A or Class B (e.g., it is integrated
directly into a flight computer as the sole communication layer without an
overlying application safety barrier), the following formal methods obligations apply:

| Obligation | Tool / Method | Scope |
|---|---|---|
| Model checking of AckTracker state machine | TLA+ or SPIN | All states and transitions in §1 |
| Model checking of RetryManager state machine | TLA+ or SPIN | All states and transitions in §2 |
| Model checking of ImpairmentEngine state machine | TLA+ or SPIN | §3 partition + delay-buffer sub-state |
| Theorem proving of Serializer bounds | Frama-C (WP plugin) or Coq | serialize() and deserialize() bounds checks |
| Proof of retry termination | TLA+ liveness property | RetryManager: every WAITING slot eventually reaches INACTIVE |

Pending reclassification, the state machine tables in §1–§3 of this document serve
as the lightweight formal specification required for Class C review.
