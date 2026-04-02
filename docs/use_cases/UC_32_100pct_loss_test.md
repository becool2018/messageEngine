# UC_32 — 100% loss configuration: every outbound message dropped, none delivered

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** User
**Requirement traceability:** REQ-5.1.3, REQ-5.2.2, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentConfig::loss_probability` is set to `1.0` and `enabled = true`. `ImpairmentEngine::process_outbound()` is called for every outbound message. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Verify that 100% loss configuration drops every outbound message without delivering any to the peer.
- **Success outcome:** `process_outbound()` returns `OK` with `*out_count = 0` for every call. No socket writes occur. The peer's receive queue remains empty.
- **Error outcomes:** None — loss is always silent.

---

## 2. Entry Points

```cpp
// ImpairmentConfig setup
cfg.impairment.enabled = true;
cfg.impairment.loss_probability = 1.0;
```

---

## 3. End-to-End Control Flow

1. User configures `loss_probability = 1.0`.
2. `PrngEngine::next_double()` draws any value `r` in `[0.0, 1.0)`.
3. **`r < loss_probability`** is always true (since `r < 1.0` always and `loss_probability = 1.0`).
4. Every envelope is dropped; `*out_count = 0`.
5. Backend's send loop runs zero iterations; no `::send()` / `::sendto()` / `inject()` calls.
6. `send_message()` returns `OK` to `DeliveryEngine::send_via_transport()`.
7. `DeliveryEngine::send()` returns `OK` to the User.
8. Peer's receive queue is never populated.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound(env, now_us, out_buf, &out_count)
 ├── is_partition_active()           <- false (no partition configured)
 └── check_loss()
      └── PrngEngine::next_double()  [r in [0, 1); always < 1.0]
      [r < 1.0 == true: drop]
      [*out_count = 0; return OK]
```

---

## 5. Key Components Involved

- **`PrngEngine::next_double()`** — Returns a value in `[0.0, 1.0)`. Since `1.0` is excluded, the condition `r < 1.0` is always true.
- **`check_loss()`** — The only gating check at `loss_probability=1.0`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `r < 1.0` | Always true: drop | Never reached |

---

## 7. Concurrency / Threading Behavior

Same as UC_13 — synchronous in calling thread; PRNG not thread-safe.

---

## 8. Memory & Ownership Semantics

Same as UC_13 — no heap allocation.

---

## 9. Error Handling Flow

- No error states. Every `send()` returns `OK` even though nothing is delivered.
- If RELIABLE_RETRY is used with 100% loss, `pump_retries()` will re-attempt but all retries will also be dropped. After `max_retries` attempts, the entry deactivates.

---

## 10. External Interactions

- None on the drop path.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | S | xorshift64(S) (one step per message) |
| `*out_count` | 0 | 0 (drop) |

---

## 12. Sequence Diagram

```
User
  -> backend.send_message(env)
       -> ImpairmentEngine::process_outbound(env, ...)
            -> check_loss()
                 -> PrngEngine::next_double()  [r < 1.0: ALWAYS TRUE]
            [*out_count = 0; return OK]
  [no ::send() called]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `cfg.impairment.loss_probability` clamped to `[0.0, 1.0]` by `impairment_config_load()` or manual assignment.

**Runtime:**
- Every `process_outbound()` call consumes one PRNG step even though the result is always DROP.

---

## 14. Known Risks / Observations

- **`send()` returning `OK` is misleading at 100% loss:** There is no feedback to the caller that the message was silently discarded.
- **PRNG consumption still occurs:** Even at 100% loss, the PRNG advances one step per message. This matters for reproducibility if tests expect a fixed PRNG sequence before/after a 100% loss phase.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `loss_probability = 1.0` is handled by `r < loss_probability` with `r` in `[0, 1)`, so `r < 1.0` is always true. The behavior is exact and does not rely on floating-point comparison edge cases at the boundary 1.0.
