# MC/DC Coverage Analysis — Five Highest-Hazard SC Functions

**Standard:** DO-178C / NASA-STD-8739.8A Modified Condition/Decision Coverage
**Method:** Test-case review per CLAUDE.md §14 ("Demonstrate MC/DC by test-case review, not tooling alone")
**Date:** 2026-03-30
**Functions in scope:** CLAUDE.md §14 lists five highest-hazard SC functions.

---

## Overview

Modified Condition/Decision Coverage (MC/DC) requires that for every **decision**
(boolean expression) in a function:

1. Every **condition** (atomic boolean sub-expression) independently affects the
   decision outcome — demonstrated by a pair of test cases that differ only in
   that condition and produce different outcomes.
2. Every reachable True and False outcome of the decision is exercised.

For compound conditions joined by short-circuit `&&` or `||`, the required MC/DC
pairs use the standard DO-178C approach:

- `A && B`: pair for A uses (A=T, B=T → T) vs (A=F, B=T → F);
  pair for B uses (A=T, B=T → T) vs (A=T, B=F → F).
- `A || B`: pair for A uses (A=T → T) vs (A=F, B=F → F);
  pair for B uses (A=F, B=T → T) vs (A=F, B=F → F).

Single-condition decisions are trivially MC/DC-compliant when both T and F
outcomes are exercised.

---

## Architectural Ceiling Note

Both `DeliveryEngine::send()` and `DeliveryEngine::receive()` contain a defensive
`if (!m_initialized) { return ERR_INVALID; }` guard (L82 and L156 respectively).
In both cases, a `NEVER_COMPILED_OUT_ASSERT(m_initialized)` immediately precedes
the guard (L79 / L153). In any build where assertions are active (all builds per
CLAUDE.md §10), the assert fires and aborts before the False-outcome path of the
guard can be reached. The True-outcome branch (`!m_initialized == true`) is
therefore architecturally unreachable and is excluded from MC/DC demonstration,
consistent with the ceiling policy established in CLAUDE.md §14.

---

## 1. `DeliveryEngine::send()` — HAZ-001, HAZ-002, HAZ-006

**File:** `src/core/DeliveryEngine.cpp` lines 77–144

### Decision table

| ID | Source line | Decision expression | Conditions | MC/DC pair — True outcome | MC/DC pair — False outcome | Test(s) |
|----|-------------|--------------------|-----------|--------------------------|-----------------------------|---------|
| S-D1 | L82 | `!m_initialized` | A = `m_initialized` | *Architecturally unreachable* — `NEVER_COMPILED_OUT_ASSERT` fires first | `test_send_best_effort` (A=T → branch skipped) | See ceiling note |
| S-D2 | L94 | `res != Result::OK` | A = `res != OK` | A=T: transport fails → return error | A=F: transport succeeds → continue | T: `test_send_transport_queue_full`; F: `test_send_best_effort` |
| S-D3 | L102–103 | `rel == RELIABLE_ACK \|\| rel == RELIABLE_RETRY` | A = `rel == RELIABLE_ACK`; B = `rel == RELIABLE_RETRY` | A=T (RELIABLE_ACK) → enters block; B=T (RELIABLE_RETRY) → enters block | A=F, B=F (BEST_EFFORT) → skips block | T(A): `test_send_reliable_ack`; T(B): `test_send_reliable_retry`; F: `test_send_best_effort` |
| S-D4 | L111 | `track_res != Result::OK` | A = `track_res != OK` | A=T: AckTracker full → log, continue | A=F: tracked → continue | T: `test_send_ack_tracker_full`; F: `test_send_reliable_ack` |
| S-D5 | L120 | `rel == RELIABLE_RETRY` | A = `rel == RELIABLE_RETRY` | A=T → schedule retry | A=F → skip retry | T: `test_send_reliable_retry`; F: `test_send_reliable_ack` |
| S-D6 | L126 | `sched_res != Result::OK` | A = `sched_res != OK` | A=T: RetryManager full → log, continue | A=F: scheduled → continue | T: `test_send_retry_manager_full`; F: `test_send_reliable_retry` |

### MC/DC independence demonstration — S-D3 (compound `||`)

`A || B` where A = `rel == RELIABLE_ACK`, B = `rel == RELIABLE_RETRY`.

| Pair | Test case | A value | B value | Outcome | Demonstrates |
|------|-----------|---------|---------|---------|--------------|
| A-pair-1 | `test_send_reliable_ack` | T | F | T (enters block) | A independently causes True |
| A-pair-2 | `test_send_best_effort` | F | F | F (skips block) | Changing A from T→F flips outcome (B held F) |
| B-pair-1 | `test_send_reliable_retry` | F | T | T (enters block) | B independently causes True |
| B-pair-2 | `test_send_best_effort` | F | F | F (skips block) | Changing B from T→F flips outcome (A held F) |

**MC/DC status: DEMONSTRATED** — all conditions independent, all outcomes exercised.

---

## 2. `DeliveryEngine::receive()` — HAZ-001, HAZ-003, HAZ-004, HAZ-005

**File:** `src/core/DeliveryEngine.cpp` lines 149–222

### Decision table

| ID | Source line | Decision expression | Conditions | MC/DC pair — True outcome | MC/DC pair — False outcome | Test(s) |
|----|-------------|--------------------|-----------|--------------------------|-----------------------------|---------|
| R-D1 | L156 | `!m_initialized` | A = `m_initialized` | *Architecturally unreachable* — `NEVER_COMPILED_OUT_ASSERT` fires first | `test_receive_data_best_effort` (A=T) | See ceiling note |
| R-D2 | L162 | `res != Result::OK` | A = `res != OK` | A=T: receive timeout → return | A=F: message received → continue | T: `test_receive_timeout`; F: `test_receive_data_best_effort` |
| R-D3 | L171 | `timestamp_expired(env.expiry_time_us, now_us)` | A = message expired | A=T → return ERR_EXPIRED | A=F → continue | T: `test_receive_expired`; F: `test_receive_data_best_effort` |
| R-D4 | L179 | `envelope_is_control(env)` | A = is control type | A=T → handle control | A=F → handle data | T: `test_receive_ack_cancels_retry`; F: `test_receive_data_best_effort` |
| R-D5 | L180 | `env.message_type == ACK` | A = `type == ACK` | A=T → process ACK (on_ack, retry cancel) | A=F → pass through (NAK/HEARTBEAT) | T: `test_receive_ack_cancels_retry`; F: `test_receive_nak_control`, `test_receive_heartbeat_control` |
| R-D6 | L201 | `rel == RELIABLE_RETRY` | A = `rel == RELIABLE_RETRY` | A=T → apply dedup | A=F → skip dedup | T: `test_receive_duplicate`; F: `test_receive_data_best_effort` |
| R-D7 | L204 | `dedup_res == ERR_DUPLICATE` | A = is duplicate | A=T → return ERR_DUPLICATE | A=F → deliver | T: `test_receive_duplicate` (second receive of same id); F: `test_receive_duplicate` (first receive of that id) |

**MC/DC status: DEMONSTRATED** — all single-condition decisions covered T and F; no compound decisions.

---

## 3. `DuplicateFilter::check_and_record()` — HAZ-003

**File:** `src/core/DuplicateFilter.cpp` lines 93–110
Note: `check_and_record()` delegates to `is_duplicate()` (L44–57) and `record()` (L63–87).
All three are analysed here as they form the single logical operation.

### Decision table

| ID | Source line | Decision expression | Conditions | MC/DC pair — True outcome | MC/DC pair — False outcome | Test(s) |
|----|-------------|--------------------|-----------|--------------------------|-----------------------------|---------|
| DF-D1 | L99 | `is_duplicate(src, msg_id)` | A = found in window | A=T → return ERR_DUPLICATE | A=F → call record(), return OK | T: `test_basic_dedup`; F: `test_not_seen` |
| DF-D2 | L48 | `m_window[i].valid && m_window[i].src == src && m_window[i].msg_id == msg_id` | A = `valid`; B = `src == src`; C = `msg_id == msg_id` | All A=T,B=T,C=T → match | See compound table below | See below |
| DF-D3 | L80 | `m_count < DEDUP_WINDOW_SIZE` | A = count below capacity | A=T → increment count | A=F → count at ceiling, skip | T: any test before window fills; F: `test_window_wraparound` |

### MC/DC independence demonstration — DF-D2 (compound `A && B && C`)

`A && B && C` where:
- A = `m_window[i].valid`
- B = `m_window[i].src == src`
- C = `m_window[i].msg_id == msg_id`

| Pair | Test case | Scenario | A | B | C | Outcome | Demonstrates |
|------|-----------|----------|---|---|---|---------|--------------|
| A-pair-T | `test_basic_dedup` | Record (src=1,id=100); search (src=1,id=100) → slot matches | T | T | T | T (match found) | — |
| A-pair-F | `test_not_seen` | Fresh filter; search any id → all slots `valid=false` | F | * | * | F (no match) | A independently causes False when B,C would be T |
| B-pair-F | `test_different_src` | Record (src=1,id=100); search (src=2,id=100) → `valid=T`, `src≠2` | T | F | T | F (no match) | B independently causes False when A=T, C=T |
| C-pair-F | `test_different_id` | Record (src=1,id=100); search (src=1,id=200) → `valid=T`, `src=1`, `id≠200` | T | T | F | F (no match) | C independently causes False when A=T, B=T |

**MC/DC status: DEMONSTRATED** — all three conditions independently affect outcome.

---

## 4. `Serializer::serialize()` — HAZ-005

**File:** `src/core/Serializer.cpp` lines 132–189

### Decision table

| ID | Source line | Decision expression | Conditions | MC/DC pair — True outcome | MC/DC pair — False outcome | Test(s) |
|----|-------------|--------------------|-----------|--------------------------|-----------------------------|---------|
| SZ-D1 | L142 | `!envelope_valid(env)` | A = envelope invalid | A=T → return ERR_INVALID | A=F → continue | T: `test_serialize_invalid_envelope`; F: `test_serialize_deserialize_basic` |
| SZ-D2 | L148 | `buf_len < required_len` | A = buffer too small | A=T → return ERR_INVALID | A=F → continue | T: `test_serialize_buffer_too_small`; F: `test_serialize_deserialize_basic` |
| SZ-D3 | L175 | `env.payload_length > 0U` | A = non-zero payload | A=T → memcpy payload | A=F → skip memcpy | T: `test_serialize_deserialize_basic`; F: `test_serialize_zero_payload` |

**MC/DC status: DEMONSTRATED** — all single-condition decisions covered T and F.

---

## 5. `Serializer::deserialize()` — HAZ-001, HAZ-005

**File:** `src/core/Serializer.cpp` lines 191–273

### Decision table

| ID | Source line | Decision expression | Conditions | MC/DC pair — True outcome | MC/DC pair — False outcome | Test(s) |
|----|-------------|--------------------|-----------|--------------------------|-----------------------------|---------|
| DS-D1 | L200 | `buf_len < WIRE_HEADER_SIZE` | A = buffer shorter than header | A=T → return ERR_INVALID | A=F → continue | T: `test_deserialize_header_too_short`; F: `test_serialize_deserialize_basic` |
| DS-D2 | L223 | `proto_ver != PROTO_VERSION` | A = protocol version byte mismatch | A=T → return ERR_INVALID | A=F → continue | T: `test_deserialize_version_mismatch`; F: `test_serialize_deserialize_basic` |
| DS-D3 | L248 | `magic_word != (static_cast<uint32_t>(PROTO_MAGIC) << 16U)` | A = frame magic mismatch or reserved bytes non-zero | A=T → return ERR_INVALID | A=F → continue | T: `test_deserialize_bad_magic`; F: `test_serialize_deserialize_basic` |
| DS-D4 | L256 | `env.payload_length > MSG_MAX_PAYLOAD_BYTES` | A = payload field too large | A=T → return ERR_INVALID | A=F → continue | T: `test_deserialize_oversized_payload_field`; F: `test_serialize_deserialize_basic` |
| DS-D5 | L262 | `buf_len < total_size` | A = buffer shorter than header+payload | A=T → return ERR_INVALID | A=F → continue | T: `test_deserialize_truncated`; F: `test_serialize_deserialize_basic` |
| DS-D6 | L267 | `env.payload_length > 0U` | A = non-zero payload | A=T → memcpy payload | A=F → skip memcpy | T: `test_serialize_deserialize_basic`; F: `test_deserialize_zero_payload` |

**MC/DC status: DEMONSTRATED** — all single-condition decisions covered T and F.

---

## Summary

| Function | Decisions | Conditions | Compound decisions | MC/DC status | New tests required |
|---|---|---|---|---|---|
| `DeliveryEngine::send` | 6 (1 unreachable) | 7 | 1 (`A\|\|B` at L102) | **DEMONSTRATED** | 0 |
| `DeliveryEngine::receive` | 7 (1 unreachable) | 7 | 0 | **DEMONSTRATED** | 0 |
| `DuplicateFilter::check_and_record` | 3 | 5 | 1 (`A&&B&&C` at L48) | **DEMONSTRATED** | 0 |
| `Serializer::serialize` | 3 | 3 | 0 | **DEMONSTRATED** | 0 |
| `Serializer::deserialize` | 6 | 6 | 0 | **DEMONSTRATED** | 0 |
| **Total** | **25 (2 unreachable)** | **28** | **2** | **ALL DEMONSTRATED** | **0** |

All MC/DC requirements are satisfied by the existing test suite. No additional test cases
are required. The two architecturally-unreachable True-outcome branches of
`!m_initialized` in `send()` and `receive()` are excluded from the demonstration on the
same basis as the ceiling branches documented in CLAUDE.md §14.

---

## Traceability

This document satisfies the MC/DC goal stated in CLAUDE.md §14 §2:

> MC/DC goal — the five highest-hazard SC functions ... Demonstrate MC/DC by
> test-case review, not tooling alone.

The test files providing coverage are:
- `tests/test_DeliveryEngine.cpp` — verifies all `DeliveryEngine::send` and `receive` decisions
- `tests/test_DuplicateFilter.cpp` — verifies all `DuplicateFilter::check_and_record` decisions
- `tests/test_Serializer.cpp` — verifies all `Serializer::serialize` and `deserialize` decisions
