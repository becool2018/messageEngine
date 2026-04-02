# UC_31 — Two runs with identical seed and message stream produce identical impairment outcomes

**HL Group:** HL-13 — Deterministic Impairment Replay
**Actor:** User
**Requirement traceability:** REQ-5.3.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** User runs the same test twice with the same `prng_seed` in `ImpairmentConfig` and the same sequence of `send_message()` calls. File: `src/platform/PrngEngine.hpp`, `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Demonstrate that every loss/jitter/duplication decision is identical across both runs — making failure scenarios reproducible and deterministic.
- **Success outcome:** Run 1 and Run 2 produce identical `process_outbound()` outcomes (same envelopes dropped, same latencies applied, same duplicates created) for matching input sequences.
- **Error outcomes:** None — this is an observational guarantee, not a function with a return value.

---

## 2. Entry Points

Observed through `ImpairmentEngine::process_outbound()` / `process_inbound()` outcomes, driven by `PrngEngine::next()` / `next_double()` / `next_range()`.

---

## 3. End-to-End Control Flow

1. **Run 1:** `backend.init(cfg)` with `cfg.impairment.prng_seed = 42` and `cfg.impairment.loss_probability = 0.3`.
2. `ImpairmentEngine::configure(cfg.impairment)` → `m_prng.seed(42)` → `m_state = 42`.
3. Message 1: `process_outbound()` → `check_loss()` → `m_prng.next_double()` draws from xorshift64(42) = S1; if `S1_double < 0.3`: drop.
4. Message 2: `process_outbound()` → `check_loss()` → xorshift64(S1) = S2; decision based on S2_double.
5. ... N messages produce N loss/forward decisions determined by the xorshift64 sequence starting at seed 42.
6. **Run 2:** Identical setup; `m_prng.seed(42)` resets `m_state = 42`.
7. Same N messages produce the same N decisions. Every dropped/forwarded/duplicated outcome matches Run 1 exactly.

---

## 4. Call Tree

```
[Run 1 and Run 2 identically:]
ImpairmentEngine::process_outbound(env_i, ...)   [ImpairmentEngine.cpp]
 └── check_loss()
      └── PrngEngine::next_double()              [PrngEngine.hpp]
           [m_state = xorshift64(m_state); return double]
```

---

## 5. Key Components Involved

- **`PrngEngine`** — xorshift64 is a fully deterministic, stateful PRNG. Given the same seed, the sequence `next()^1, next()^2, ...` is always identical.
- **`ImpairmentEngine`** — All impairment decisions (loss, dup, jitter) use `m_prng.next_double()` or `m_prng.next_range()`. No OS entropy or time-based randomness is used.

---

## 6. Branching Logic / Decision Points

This UC is about verifying determinism; the branching logic is identical to UC_13, UC_14, UC_16. The key invariant is that `PrngEngine::next()` is the only source of randomness, and it is fully seeded from `ImpairmentConfig::prng_seed`.

---

## 7. Concurrency / Threading Behavior

- Single-threaded assumption for reproducibility. If multiple threads call `process_outbound()` concurrently, the `m_state` interleaving is non-deterministic.

---

## 8. Memory & Ownership Semantics

- `PrngEngine::m_state` — 8-byte member. The only state that determines the sequence.

---

## 9. Error Handling Flow

- No error states. Determinism is a property, not a function.

---

## 10. External Interactions

- None — xorshift64 uses no OS calls, no time, no hardware randomness.

---

## 11. State Changes / Side Effects

- `PrngEngine::m_state` advances identically across both runs when driven by the same call sequence.

---

## 12. Sequence Diagram

```
Run 1:
  seed(42) -> m_state=42
  Message 1: next_double() -> xorshift64(42)=S1; S1_double=0.25 < 0.3 -> DROP
  Message 2: next_double() -> xorshift64(S1)=S2; S2_double=0.71 >= 0.3 -> FORWARD

Run 2 (same seed):
  seed(42) -> m_state=42
  Message 1: next_double() -> xorshift64(42)=S1; S1_double=0.25 < 0.3 -> DROP  [IDENTICAL]
  Message 2: next_double() -> xorshift64(S1)=S2; S2_double=0.71 >= 0.3 -> FORWARD [IDENTICAL]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- Both runs use the same `prng_seed` and the same `ImpairmentConfig` values.
- The same number and sequence of `process_outbound()` / `process_inbound()` calls are made in both runs (including all impairment check calls in order).

**Runtime:**
- Any deviation in the call sequence (e.g., different number of messages, different impairment settings) breaks reproducibility.

---

## 14. Known Risks / Observations

- **Call-sequence dependency:** Reproducibility requires identical call sequences. If the application sends different numbers of messages between runs, the PRNG sequences diverge.
- **Platform independence:** xorshift64 uses only integer arithmetic; results are identical across compilers and platforms for the same seed.
- **`prng_seed=0` edge case:** Both runs use seed 1 (coerced from 0), producing identical sequences.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Every impairment check (loss, dup, jitter) makes exactly one `PrngEngine::next()` call per `process_outbound()` invocation. If the number of draws per call varies based on configuration, the total draw count per message may differ.
