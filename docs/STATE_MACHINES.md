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
| `ACKED` | `sweep_expired()` | — | `FREE` | Decrement m_count (guarded: only if m_count > 0U — INT30-C unsigned underflow guard) |

### Invariants

- `0 ≤ m_count ≤ ACK_TRACKER_CAPACITY` at all times.
- A slot in `ACKED` state is freed on the very next `sweep_expired()` call.
- A slot in `PENDING` state is freed by: (a) `sweep_expired()` when the deadline passes — copied to `expired_buf` if space available; or (b) `cancel()` on send-failure rollback — freed directly to `FREE` with no stat bump and no copy to `expired_buf`.
- `on_ack()` never decrements `m_count` — only `sweep_expired()` and `cancel()` do.
- `now_us` must be non-zero at entry. A backward `now_us` (now < m_last_sweep_us) triggers WARNING_HI and is **clamped** to `m_last_sweep_us`; processing continues with the clamped value (G-1 behavior, commit d7584dd). This replaces the prior `NEVER_COMPILED_OUT_ASSERT` which would have aborted the process.

> **Note:** `cancel()` is a rollback-only path, callable exclusively from `DeliveryEngine::send()` on transport failure before any wire I/O. It must not be called after a message has been delivered to the wire.

> **G-1 monotonic behavior change (commit d7584dd):** Prior to G-1, a backward timestamp triggered `NEVER_COMPILED_OUT_ASSERT(now_us >= m_last_sweep_us)` and aborted the process. G-1 replaced this with a defensive clamp + WARNING_HI. The invariant is preserved (slots expire correctly at worst one sweep late). See SECURITY_ASSUMPTIONS.md §1.

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
| `DUE` | true | `next_retry_us ≤ now_us` AND `retry_count < max_retries` AND `expiry_us > now_us` | Ready to retransmit. Note: `collect_due()` uses a two-phase design — Phase 1 (`reap_terminated_slots()`) reaps EXPIRED and EXHAUSTED slots first; Phase 2 checks `next_retry_us <= now_us`. The `DUE` guard is a conceptual composite evaluated across both phases, not a single compound condition in one branch. |
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
| `WAITING` | `on_ack(src, msg_id)` | src and msg_id match | `INACTIVE` | Decrement m_count; increment acks_received; log INFO |
| `WAITING` | `on_ack(src, msg_id)` | src or msg_id does not match | `WAITING` | Returns `ERR_INVALID`; log `WARNING_LO`; no state change. |
| `WAITING` | `cancel(src, msg_id)` | src and msg_id match slot | `INACTIVE` | Deactivate slot; decrement m_count; do NOT increment acks_received; log INFO. Returns `ERR_INVALID` if no match. |
| `DUE` | `collect_due(now, buf)` | buf has space | `WAITING` | Copy to buf; increment retry_count; advance backoff; set next_retry_us |
| `DUE` | `collect_due(now, buf)` | buf full | `DUE` | Not collected this sweep; will retry on next call. Note: slot's `next_retry_us` is not advanced when buffer is full; the slot will be collected on the next `collect_due()` call that has buffer capacity. |

### Invariants

- `0 ≤ m_count ≤ ACK_TRACKER_CAPACITY` at all times. (RetryManager uses `ACK_TRACKER_CAPACITY` — the same constant as AckTracker — for its slot array size.)
- `backoff_ms` is monotonically non-decreasing per slot, capped at 60 000 ms.
- `retry_count` is monotonically non-decreasing per slot.
- A slot transitions to `INACTIVE` only through: ACK received, exhausted, or expired.
- The `schedule()` call sets `next_retry_us = now_us` so the first retry fires immediately.
- `now_us` must be non-zero at entry. A backward `now_us` (now < m_last_collect_us) triggers WARNING_HI and is **clamped** to `m_last_collect_us`; processing continues with the clamped value (G-1 behavior, commit d7584dd). This replaces the prior `NEVER_COMPILED_OUT_ASSERT`.

> **Defect fixed (DEF-003-1):** The source_id mismatch described for AckTracker §1 applied equally to RetryManager. `DeliveryEngine::receive()` now passes `env.destination_id` (local node ID) to `RetryManager::on_ack()`, which correctly matches the stored `slot.env.source_id` (also the local node ID set at `schedule()` time). ACK receipt now cancels retry slots via `on_ack()` in both AckTracker and RetryManager as intended.

> **G-1 monotonic behavior change (commit d7584dd):** Prior to G-1, a backward timestamp triggered `NEVER_COMPILED_OUT_ASSERT(now_us >= m_last_collect_us)` and aborted the process. G-1 replaced this with a defensive clamp + WARNING_HI. See SECURITY_ASSUMPTIONS.md §1.

> **`cancel()` rollback path:** `cancel()` is used on the transport send-failure rollback path. It deactivates a WAITING slot without recording it as an ACK receipt, preserving the accuracy of the `acks_received` statistic.

> **Security note (REQ-3.2.11):** `RetryManager::on_ack()` uses plain `==` equality for the (source_id, message_id) slot lookup, not the constant-time `ct_id_pair_equal()` used by `AckTracker::on_ack()`. This divergence is intentional: the RetryManager lookup is protected by the upstream `AckTracker` which performs the constant-time check before an ACK reaches the retry path. A direct timing attack on `RetryManager::on_ack()` requires an adversary who can already forge ACKs past `AckTracker`, which is the higher-value target. This justification is recorded here per REQ-3.2.11.

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
| `UNINIT` | `!m_initialized` | `init()` not yet called; all operations assert-fail. In debug/test builds, pre-init calls abort via `NEVER_COMPILED_OUT_ASSERT`; in production builds, they trigger `IResetHandler::on_fatal_assert()` controlled reset. |
| `READY` | `m_initialized` | Ready to process messages |

**Dimension 2 — Partition (within `READY`):**

| State | Fields | Meaning |
|-------|--------|---------|
| `NO_PARTITION` | `m_partition_active == false` | Link is up; messages pass through impairment pipeline |
| `PARTITION_ACTIVE` | `m_partition_active == true`, `m_partition_start_us` = start timestamp | Simulated link outage; all outbound messages dropped with `ERR_IO` |

**Partition state fields:**

| Field | Description |
|-------|-------------|
| `m_partition_active` | True when a simulated link outage is in progress |
| `m_partition_start_us` | Start timestamp of the current partition interval, set when partition becomes active; used for diagnostics |
| `m_next_partition_event_us` | Timestamp of the next partition state transition (gap→active or active→gap) |

Transitions are driven by `m_next_partition_event_us` (uint64_t) and the `now_us` argument passed to each `process_outbound()` call. The initial state is always `NO_PARTITION`; the first active partition begins after `partition_gap_ms` elapses from the first call.

### Transition Table — Initialisation Dimension

| Current State | Event | Next State | Action |
|---------------|-------|------------|--------|
| `UNINIT` | `init(cfg)` | `READY` | Seed PRNG; zero delay buffer; zero reorder buffer; reset reorder count; reset `m_partition_active` to false; reset `m_partition_start_us` to 0; reset `m_next_partition_event_us` to 0; zero stats; set `m_initialized` |
| `READY` | Any operation | `READY` | Normal processing |

### Transition Table — Partition Dimension (within READY)

`is_partition_active(now_us)` is the sole transition function. Its internal logic:

| Condition | Action | Returns |
|-----------|--------|---------|
| `!m_cfg.partition_enabled` | No state change | `false` |
| `m_next_partition_event_us == 0` (first call) | `m_partition_active = false`; `m_next_partition_event_us = sat_add_us(now_us, partition_gap_ms)` ¹ | `false` |
| `!m_partition_active && now_us >= m_next_partition_event_us` | `m_partition_active = true`; `m_partition_start_us = now_us`; `m_next_partition_event_us = sat_add_us(now_us, partition_duration_ms)` ¹; log `WARNING_LO` | `true` |
| `m_partition_active && now_us >= m_next_partition_event_us` | `m_partition_active = false`; `m_next_partition_event_us = sat_add_us(now_us, partition_gap_ms)` ¹; log `WARNING_LO` | `false` |
| Otherwise | No state change | `m_partition_active` |

Effect on `process_outbound()`:

| `is_partition_active()` | `process_outbound()` action |
|-------------------------|----------------------------|
| `true` | Drop message; log `WARNING_LO`; return `ERR_IO` |
| `false` | Apply loss/jitter/duplication; queue to delay buffer |

> ¹ `sat_add_us(now_us, delta_ms)` computes `now_us + delta_ms × 1000` with CERT INT30-C unsigned saturation: if the addition would overflow `UINT64_MAX`, the result is clamped to `UINT64_MAX` (effectively "never fires"), preventing deadline wrap-around (added in commit 18e6d1a).

> **Edge case eliminated:** `partition_gap_ms == 0` with `partition_enabled == true` is now rejected by `ImpairmentEngine::init()` via `NEVER_COMPILED_OUT_ASSERT(!cfg.partition_enabled || cfg.partition_gap_ms > 0U)`. Callers that supply this combination receive a FATAL assertion and `init()` returns without setting `m_initialized`, so the engine cannot be used. The prior silent behaviour (permanent partition on the second call) is no longer reachable.

> **Unicast routing note:** Partition drops apply to both broadcast (`destination_id == 0`) and unicast (`destination_id > 0`) paths equally. The unicast slot lookup (§6 below) occurs before the partition check in the send path in the calling TcpBackend/TlsTcpBackend, but the partition causes the send to be dropped regardless of routing result — a routable unicast slot is no protection against a simulated link outage.

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
- When `m_cfg.enabled == true`, `process_outbound()` checks `is_partition_active()`
  before any queuing decision. When `m_cfg.enabled == false`, the `!m_cfg.enabled`
  fast-path runs first and the partition check is bypassed — messages pass through
  directly (subject to buffer-full). No message is queued to the delay buffer during
  an active partition (enabled-mode only).
- If `m_cfg.enabled == false`, messages are queued to the delay buffer with
  `release_us = now_us` (immediate pass-through), preserving the pipeline interface.
  If the delay buffer is full, `process_outbound()` returns `ERR_FULL` even in
  disabled mode.
- **Partition drops apply symmetrically to inbound and outbound:** When `m_cfg.enabled == true`, both `process_outbound()` and `process_inbound()` check `is_partition_active()` before processing; a partition drops traffic in both directions. When `m_cfg.enabled == false`, the `!m_cfg.enabled` fast-path runs first in both functions, bypassing the partition check entirely.

---

## Appendix A — Software Classification and Formal Methods Applicability

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
| Model checking of ReassemblyBuffer state machine | TLA+ or SPIN | All states and transitions in §4 |
| Model checking of OrderingBuffer state machine | TLA+ or SPIN | All states and transitions in §5 |
| Theorem proving of Serializer bounds | Frama-C (WP plugin) or Coq | serialize() and deserialize() bounds checks |
| Proof of retry termination | TLA+ liveness property | RetryManager: every WAITING slot eventually reaches INACTIVE |

Pending reclassification, the state machine tables in §§1–6 of this document serve
as the lightweight formal specification required for Class C review.

---

## §4 ReassemblyBuffer State Machine

**Source:** src/core/ReassemblyBuffer.hpp / ReassemblyBuffer.cpp
**Purpose:** Reassembles fragmented logical messages from up to FRAG_MAX_COUNT wire frames.
**Hazards mitigated:** HAZ-003 (duplicate delivery via corrupt reassembly), HAZ-006 (slot exhaustion)

### States (per ReassemblySlot)

| State | Meaning |
|-------|---------|
| `INACTIVE` | Slot is free; no message in progress |
| `COLLECTING` | At least one fragment received; waiting for remaining fragments |
| `COMPLETE` | All fragments received (transient — slot freed immediately after assembly) |

### Transition Table

| From | Event | Guard | To | Action |
|------|-------|-------|----|--------|
| Any | `ingest(frag)` with `fragment_count <= 1` | — | INACTIVE (fast path) | Copy frag directly to logical_out; return OK |
| INACTIVE | `ingest(frag)` with `fragment_count > 1` | Slot available | COLLECTING | `open_slot()`: record message_id, source_id, fragment_count, total_length |
| INACTIVE | `ingest(frag)` with `fragment_count > 1` | No free slot | INACTIVE | Return ERR_FULL |
| COLLECTING | `ingest(frag)` — same (source_id, message_id) | `fragment_count` matches | COLLECTING | `record_fragment()`: copy payload slice; set bitmask bit |
| COLLECTING | `ingest(frag)` — same (source_id, message_id) | `fragment_count` mismatch | COLLECTING | Return ERR_INVALID; slot unchanged |
| COLLECTING | `ingest(frag)` — duplicate fragment_index | bit already set | COLLECTING | Return ERR_AGAIN (silently discard) |
| COLLECTING | `ingest(frag)` — completing fragment | All bits set | COMPLETE → INACTIVE | `assemble_and_free()`: copy payload to logical_out; free slot; return OK |
| COLLECTING | `sweep_expired(now_us)` | expiry_us != 0 && expiry_us <= now_us | INACTIVE | Clear received_mask; set active=false |

### Invariants

- At most REASSEMBLY_SLOT_COUNT slots can be COLLECTING simultaneously.
- `received_mask` has at most `expected_count` bits set (bits 0..FRAG_MAX_COUNT-1).
- Once COMPLETE, slot is immediately freed; no slot persists in COMPLETE state.
- Fragment payload is placed at byte offset `fragment_index * FRAG_MAX_PAYLOAD_BYTES` in the accumulation buffer; this is a static placement rule with no overlap for valid indices.

---

## §5 OrderingBuffer State Machine

**Source:** src/core/OrderingBuffer.hpp / OrderingBuffer.cpp
**Purpose:** Per-peer in-order delivery gate; holds out-of-order messages until gaps are filled.
**Hazards mitigated:** HAZ-001 (misordered delivery), HAZ-006 (hold buffer exhaustion), HAZ-016 (ordering gate stall on peer reconnect)

### States (per HoldSlot)

| State | Meaning |
|-------|---------|
| `FREE` | Hold slot is not in use |
| `HELD` | Out-of-order message waiting for its sequence gap to be filled |

### States (per PeerState)

| Field | Meaning |
|-------|---------|
| `next_expected_seq` | The sequence number the peer must deliver next (starts at 1) |
| `active` | Whether this peer slot is tracking any source |

### Transition Table (ingest)

| Event | Guard | Result | Action |
|-------|-------|--------|--------|
| `ingest(msg)` — control message | `envelope_is_control(msg)` | OK | Copy to out; bypass all ordering logic |
| `ingest(msg)` — sequence_num == 0 | UNORDERED marker | OK | Copy to out; bypass ordering gate |
| `ingest(msg)` — `seq == expected` | In-order | OK | Copy to out; advance next_expected |
| `ingest(msg)` — `seq < expected` | Already delivered | ERR_AGAIN | Silently discard (duplicate sequence) |
| `ingest(msg)` — `seq > expected` | Out-of-order | ERR_AGAIN | Store in free HoldSlot; return ERR_AGAIN |
| `ingest(msg)` — `seq > expected` | No free HoldSlot | ERR_FULL | Return ERR_FULL; message dropped |

### Transition Table (try_release_next)

| Event | Guard | Result | Action |
|-------|-------|--------|--------|
| `try_release_next(src)` | Held message with `seq == expected` for src | OK | Copy to out; advance next_expected; free HoldSlot |
| `try_release_next(src)` | No held message matches | ERR_AGAIN | No action |

### Transition Table (advance_sequence)

| Event | Guard | Action |
|-------|-------|--------|
| `advance_sequence(src, up_to_seq)` | `up_to_seq > next_expected` | Advance next_expected to up_to_seq (gap-skip on timeout) |
| `advance_sequence(src, up_to_seq)` | `up_to_seq <= next_expected` | No-op |

### Transition Table (reset_peer — REQ-3.3.6)

Called by `DeliveryEngine::reset_peer_ordering()` when a peer reconnect is detected
(HELLO frame received via `TransportInterface::pop_hello_peer()`).

| Event | Guard | Action |
|-------|-------|--------|
| `reset_peer(src)` | Peer slot found (active, src matches) | Set `next_expected_seq = 1`; free all HoldSlots where `env.source_id == src`; log WARNING_HI |
| `reset_peer(src)` | No active peer slot for src | No-op (idempotent; not an error) |

### Invariants

- At most ORDERING_HOLD_COUNT messages can be HELD simultaneously (across all peers).
- At most ORDERING_PEER_COUNT peers can be tracked simultaneously.
- Control messages and UNORDERED messages (sequence_num == 0) are never held.
- next_expected_seq is monotonically non-decreasing for each peer **within a session**; `reset_peer()` resets it to 1 at reconnect to start a new session.
- A held message is released at most once (HoldSlot freed immediately on release).
- After `reset_peer(src)`, no HoldSlot with `env.source_id == src` remains active.

---

## §6 TCP/TLS Server Client Registration State Machine

**Component:** TcpBackend (server mode) / TlsTcpBackend (server mode)
**Hazards mitigated:** HAZ-001 (wrong-node delivery)
**Requirements:** REQ-6.1.8, REQ-6.1.9

### States

| State | Description |
|---|---|
| UNREGISTERED | Client socket connected; no HELLO received; NodeId unknown. Unicast sends to this client will fail with ERR_INVALID. |
| REGISTERED | HELLO received; NodeId recorded in slot table. Unicast sends to this NodeId route to this slot. |

### Transitions

| From State | Event | Guard | To State | Action |
|---|---|---|---|---|
| UNREGISTERED | HELLO frame received | source_id != NODE_ID_INVALID AND source_id not already registered in any other slot | REGISTERED | Store source_id → slot in m_client_node_ids[]; log INFO "Registered client NodeId N at slot S" |
| UNREGISTERED | HELLO frame received | source_id already registered in a different slot | (slot evicted) | Evict NEW slot: call close_and_evict_slot() / remove_client(); log WARNING_HI. Existing registration is preserved. |
| REGISTERED | HELLO frame received (re-registration) | source_id == existing entry for this slot | REGISTERED | Idempotent; no change. Log INFO. |
| REGISTERED | HELLO frame received (duplicate HELLO on same slot) | second HELLO, source_id any | (slot evicted) | Evict this slot: call close_and_evict_slot() / remove_client(); log WARNING_HI. (REQ-6.1.8 one-HELLO-per-connection rule.) |
| REGISTERED | Client disconnects | — | (slot freed) | Clear m_client_node_ids[slot] = NODE_ID_INVALID; compact table |
| UNREGISTERED | Client disconnects | — | (slot freed) | Clear slot; compact table |

### Invariants

1. `m_client_node_ids[i] == NODE_ID_INVALID` for all slots `i >= m_client_count`.
2. **No two active slots hold the same non-zero NodeId at any time.** (TcpBackend: G-3 + F-13; TlsTcpBackend: SEC-012 / DEF-018-4.) When a new HELLO arrives claiming a NodeId already present in another slot, the **new** connection is evicted; the existing registration is preserved.
3. HELLO frames never reach DeliveryEngine::receive(); they are consumed at the transport layer and return ERR_AGAIN to the poll loop.
4. A slot that sends a second HELLO is evicted immediately (TcpBackend: G-3; TlsTcpBackend: SEC-013 / DEF-018-5).

> **SEC-012 (TlsTcpBackend, commit 4cda101):** Prior to SEC-012, TlsTcpBackend's `handle_hello_frame()` did not scan other slots for duplicate NodeId. A second TLS connection could claim a NodeId already registered by an active peer, hijacking the routing table entry and allowing source_id spoofing. SEC-012 adds the same cross-slot scan that TcpBackend received in G-3 + F-13.

> **TcpBackend vs TlsTcpBackend invariant alignment:** Both backends now enforce identical invariants for client registration. The TlsTcpBackend uses `remove_client(idx)` (in-place TLS context re-init + slot deactivation) rather than `close_and_evict_slot()` / array compaction, but the observable invariant is the same.

### Guard on outbound unicast (REQ-6.1.9)

```
send_message(env):
  if env.destination_id == NODE_ID_INVALID:
      → broadcast: send to all slots with m_client_fds[i] >= 0
  else:
      slot = find_client_slot(env.destination_id)
      if slot == MAX_TCP_CONNECTIONS:
          → ERR_INVALID + WARNING_HI (no registered client for that NodeId)
      else:
          → send to m_client_fds[slot] only
```
