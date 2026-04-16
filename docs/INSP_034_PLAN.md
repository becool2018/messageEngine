# INSP-034 Implementation Plan — Complete REQ-3.3.6 Reconnect Reset

**Status:** Approved, not yet started
**Date drafted:** 2026-04-16
**Inspection record:** docs/DEFECT_LOG.md — INSP-034
**Prerequisite:** INSP-033 (outbound seq counter reset, already committed on fix/reconnect-seq-reset-2026-04)

---

## Background

INSP-033 fixed the missing outbound sequence counter reset in `reset_peer_ordering()`.
Runtime testing revealed the fix is incomplete: stale `AckTracker` and `RetryManager`
entries for the reconnecting peer are not cancelled. They retry pre-reset envelopes
(with old sequence numbers) on the new connection, inserting old sequence numbers into
the new session's ordered delivery stream. Observed: client 2's 5th echo was a stale
retry from client 1's session (`msg_id=…768`), not its own echo (`msg_id=…773`).

---

## Code Changes (5 files)

### `src/core/Types.hpp`
Add `RECONNECT_CANCEL` to the `ObsEvent` enum.

### `src/core/AckTracker.hpp` + `AckTracker.cpp`
New public SC method `cancel_peer(NodeId dst)`:
- Bounded loop over slots (≤ `ACK_TRACKER_CAPACITY`)
- Cancels all active slots where destination matches `dst`
- Returns count of entries cancelled
- SC annotation: HAZ-001, HAZ-016

### `src/core/RetryManager.hpp` + `RetryManager.cpp`
New public SC method `cancel_peer(NodeId dst)`:
- Bounded loop over entries (≤ `MAX_RETRY_COUNT`)
- Cancels all active entries where destination matches `dst`
- Returns count of entries cancelled
- SC annotation: HAZ-016

### `src/core/DeliveryEngine.cpp` — `reset_peer_ordering(src)`
After the existing `m_seq_state` reset, two new calls:
- `m_ack_tracker.cancel_peer(src)` → log `WARN_HI` per cancelled entry:
  "Cancelled in-flight message_id=X to dst=Y due to peer reconnect (REQ-3.3.6)";
  push `RECONNECT_CANCEL` event to observability ring
- `m_retry.cancel_peer(src)` → same `WARN_HI` log and `RECONNECT_CANCEL` event per entry

---

## Test Changes (3 files)

### `tests/test_AckTracker.cpp`
- `cancel_peer()` cancels matching active slots and returns correct count
- `cancel_peer()` is no-op when no matching slot exists
  (covers the Class B loop-exhaustion gap identified in INSP-033 coverage note)

### `tests/test_RetryManager.cpp`
- Same two cases for `RetryManager::cancel_peer()`

### `tests/test_DeliveryEngine.cpp`
- `reset_peer_ordering()` on a peer with in-flight messages fires `RECONNECT_CANCEL`
  events and `WARN_HI` logs
- `reset_peer_ordering()` on a peer with no in-flight messages is a no-op
  (also covers the Class B loop-exhaustion gap from INSP-033)
- Runtime scenario: server + two clients; confirm client 2 gets 5/5 from
  new-session messages only (no stale retries from client 1's session)

---

## Documentation Changes (7 files)

| Document | Change |
|---|---|
| `docs/HAZARD_ANALYSIS.md` | HAZ-016 mitigation updated; FMEA rows for AckTracker/RetryManager updated |
| `docs/STATE_MACHINES.md` | AckTracker state machine: add `RECONNECT_CANCEL` transition from `PENDING` |
| `docs/WCET_ANALYSIS.md` | `reset_peer_ordering()` op count updated (three bounded loops now) |
| `docs/COVERAGE_CEILINGS.md` | New branches in DeliveryEngine, AckTracker, RetryManager |
| `docs/TRACEABILITY_MATRIX.md` | Sync note |
| `docs/use_cases/UC_72_peer_reconnect_ordering_reset.md` | Add cancellation to control flow, call tree, state changes, sequence diagram |
| `docs/DEFECT_LOG.md` | Close INSP-034 |

---

## What This Does NOT Trigger

- **Stress tests** — no capacity constant changes; `cancel_peer()` loops are bounded
  by existing constants (`ACK_TRACKER_CAPACITY`, `MAX_RETRY_COUNT`)
- **INSP-033 amendment** — INSP-033 remains closed as-is; post-close finding note
  already appended to its record in DEFECT_LOG.md

---

## Design Decisions (resolved)

- **Public methods vs internal access:** Public `cancel_peer(NodeId dst)` on both
  `AckTracker` and `RetryManager`. Consistent with existing API pattern; independently
  testable; preserves encapsulation.
- **SEND_FAIL vs RECONNECT_CANCEL:** `RECONNECT_CANCEL` chosen. The send succeeded;
  what failed is delivery confirmation due to peer reset. SEND_FAIL would be
  semantically incorrect (message reached the wire) and misleading to operators.
- **INSP-033 vs INSP-034:** Separate INSP-034. INSP-033 was closed PASS for a
  bounded scope; amending a closed inspection muddies the audit trail.
