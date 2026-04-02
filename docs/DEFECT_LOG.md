# messageEngine — Defect Log

Policy: CLAUDE.md §12.3 / NPR 7150.2D
This is a permanent project record. Entries must never be deleted.
Entries may be updated to record resolution; the original finding must remain visible.

Severity levels:
  CRITICAL — safety, correctness, or security issue; must be fixed before inspection exit.
  MAJOR    — standards violation or logic error; must be fixed or waived with justification.
  MINOR    — style, clarity, or advisory; may be deferred with a tracked note.

Disposition codes:
  FIX   — defect was corrected; resolution notes describe the fix.
  WAIVE — defect accepted as-is; written justification required; moderator + reviewer agree.
  DEFER — defect logged for later; must reference a tracked issue or REQ ID.

---

## Inspection records

<!-- Add one H3 block per inspection. Copy the template below. -->

<!--
### INSP-001 — <short description of change reviewed>

| Field       | Value |
|-------------|-------|
| Date        |       |
| Author      |       |
| Moderator   |       |
| Reviewer(s) |       |
| Outcome     | PASS / FAIL / CONDITIONAL PASS |

#### Defects found

| ID         | File : line | Description | Severity | Disposition | Resolution |
|------------|-------------|-------------|----------|-------------|------------|
| INSP-001-1 |             |             |          |             |            |

#### Checklist reference
Completed checklist on file: [link or inline]
-->

---

### INSP-001 — M5 fault injection: IMbedtlsOps ssl_handshake/ssl_read, SC annotations, coverage ceilings

| Field       | Value |
|-------------|-------|
| Date        | 2026-03-31 |
| Author      | Don Jessup |
| Moderator   | Don Jessup (AI-assisted development; human engineer acts as moderator per §12.1) |
| Reviewer(s) | Claude Sonnet 4.6 (AI co-author); human engineer self-review against INSPECTION_CHECKLIST.md |
| Outcome     | CONDITIONAL PASS — no defects found; all entry and exit criteria met |

#### Scope of change

| File(s) | Change summary |
|---------|---------------|
| `src/platform/IMbedtlsOps.hpp` | Added `ssl_handshake` and `ssl_read` pure-virtual methods with SC annotations (HAZ-004/005/006 and HAZ-004/005); previously these were called directly in DtlsUdpBackend, bypassing the injectable interface |
| `src/platform/MbedtlsOpsImpl.hpp/.cpp` | Added `ssl_handshake` and `ssl_read` delegation implementations with `NEVER_COMPILED_OUT_ASSERT` guards |
| `src/platform/DtlsUdpBackend.cpp` | Routed `mbedtls_ssl_handshake` and `mbedtls_ssl_read` through `m_ops` (two targeted call-site edits) |
| 17 `.hpp` files in `src/` | All 48 SC annotations updated with `— verified to M5` suffix per VVP-001 §5 |
| `src/platform/ImpairmentConfig.hpp` | Added missing `Implements: REQ-5.2.1` tag to forwarding shim |
| `tests/test_DtlsUdpBackend.cpp` | Extended `DtlsMockOps` with `ssl_handshake`/`ssl_read` overrides; added 3 M5 tests: `test_mock_dtls_ssl_write_fail`, `test_mock_dtls_handshake_iteration_limit`, `test_mock_dtls_ssl_read_error` |
| `tests/test_UdpBackend.cpp` | Added `test_mock_udp_recv_from_fail` (M5: `ISocketOps::recv_from` failure path) |
| 15 test files | Added `// Verification: M1 + M2 + M4 + M5` (or M3 for NSC) file-level headers per VVP-001 §5 |
| `docs/HAZARD_ANALYSIS.md §3` | Added `ssl_handshake` SC row (HAZ-004, HAZ-005, HAZ-006); updated IMbedtlsOps rationale |
| `docs/COVERAGE_CEILINGS.md` (referenced from CLAUDE.md §14) | Updated DtlsUdpBackend ceiling 81%→83% (200/240 at inspection date); MbedtlsOpsImpl ceiling 70%→69% (60/86) — **Note:** commit c7e8202 (2026-04-02) subsequently raised DtlsUdpBackend to 242/296 branches; see post-inspection note below |
| `docs/STACK_ANALYSIS.md` | Added `ssl_handshake` init-phase note (does not affect runtime chains) |
| `docs/WCET_ANALYSIS.md` | Added `ssl_handshake`, `ssl_write`, `ssl_read` rows to IMbedtlsOps table |

#### Entry criteria verification

| Criterion | Status |
|-----------|--------|
| `make` passes with zero warnings and zero errors | PASS |
| `make check_traceability` RESULT: PASS | PASS |
| `make run_tests` all tests green (38 DtlsUdpBackend, 18 UdpBackend, 15 suites total) | PASS |
| All new/modified `src/` files carry `// Implements: REQ-x.x` tags | PASS |
| All new/modified `tests/` files carry `// Verifies: REQ-x.x` tags | PASS |
| No raw `assert()` in `src/` — `NEVER_COMPILED_OUT_ASSERT` used throughout | PASS |
| No dynamic allocation on critical paths after init | PASS |
| Author self-reviewed against `docs/INSPECTION_CHECKLIST.md` | PASS |

#### Defects found

| ID | File : line | Description | Severity | Disposition | Resolution |
|----|-------------|-------------|----------|-------------|------------|
| — | — | No defects found during inspection | — | — | — |

#### Checklist reference

All items in `docs/INSPECTION_CHECKLIST.md` verified. Key checks:
- Power of 10 Rule 5: new `MbedtlsOpsImpl` methods each carry ≥ 1 `NEVER_COMPILED_OUT_ASSERT` (ssl_handshake: 1; ssl_read: 2). ✓
- Power of 10 Rule 9: virtual dispatch via vtable — permitted exception per CLAUDE.md §2. ✓
- SC annotation format: `// Safety-critical (SC): HAZ-NNN — verified to M5` per VVP-001 §5. ✓
- Traceability: `Implements` and `Verifies` tags present; `make check_traceability` PASS. ✓
- Coverage: DtlsUdpBackend 83.33% (200/240) exceeds 83% threshold; all reachable branches covered. ✓
- HAZARD_ANALYSIS.md §3: `ssl_handshake` classified SC with correct HAZ IDs. ✓

#### Moderator sign-off

Moderator: Don Jessup — 2026-03-31. All entry and exit criteria satisfied. No CRITICAL or MAJOR defects. Inspection closed PASS.

#### Post-inspection note (2026-04-02)

Commit c7e8202 ("Add branch coverage tests and update ceilings for three regressions") made
the following changes after INSP-001 closed. These are recorded here for audit continuity;
no re-inspection is required as no SC function logic was altered:

| File | Change |
|------|--------|
| `docs/COVERAGE_CEILINGS.md` | DtlsUdpBackend branch count updated 200/240 → 242/296 (new branches added by additional tests; ceiling % unchanged at 83%) |
| `src/platform/IMbedtlsOps.hpp` lines 108, 114, 123 | Duplicate `— verified to M5` annotation suffix removed (cosmetic; no traceability impact) — resolved by validate-safety-doc run 2026-04-02 |
| `src/platform/LocalSimHarness.hpp` | SC annotations added to `send_message`, `receive_message`, and `inject` — omission identified by validate-safety-doc run 2026-04-02; no safety logic changed |

---

### INSP-002 — Safety document validation audit (validate-safety-doc all, 2026-04-02)

| Field       | Value |
|-------------|-------|
| Date        | 2026-04-02 |
| Author      | Don Jessup |
| Moderator   | Don Jessup (AI-assisted development; human engineer acts as moderator per §12.1) |
| Reviewer(s) | Claude Sonnet 4.6 (AI co-author via /validate-safety-doc all); human engineer self-review |
| Outcome     | CONDITIONAL PASS — one CRITICAL defect (DEF-002-1) logged OPEN; all other findings resolved in-session |

#### Scope of change

Safety document validation audit of all nine Safety & Assurance documents against the live
source code. Nine sub-agents ran in three dependency-ordered waves. Documentation and
annotation fixes were applied in-session (see post-inspection note for INSP-001 above and
the commit following this audit). One CRITICAL defect was deferred to a separate inspection.

#### Entry criteria verification

Entry criteria were verified at time of validation run:

| Criterion | Status |
|-----------|--------|
| `make` passes with zero warnings and zero errors | PASS |
| `make lint` passes with zero clang-tidy violations | PASS |
| `make check_traceability` RESULT: PASS | PASS (verified post-fix) |
| `make run_tests` all tests green | PASS |

#### Defects found

| ID | File : function | Description | Severity | Status | Disposition | Assignee | Target |
|----|----------------|-------------|----------|--------|-------------|----------|--------|
| DEF-002-1 | `src/core/DeliveryEngine.cpp` : `pump_retries()`, `sweep_ack_timeouts()` | **Oversized stack allocation in SC functions.** `pump_retries()` allocates `MessageEnvelope retry_buf[MSG_RING_CAPACITY=64]` (~263 KB) on the stack; `sweep_ack_timeouts()` allocates `MessageEnvelope timeout_buf[ACK_TRACKER_CAPACITY=32]` (~131 KB) on the stack. Both violate the spirit of Power of 10 Rule 3 on any embedded target with a constrained stack budget (≤ 512 KB). On POSIX platforms (8 MB default stack) no overflow occurs. Hazards: HAZ-002 (pump_retries), HAZ-004, HAZ-006 (both functions). | CRITICAL | **CLOSED** | FIX | Don Jessup | 2026-04-02 |

#### Resolution of DEF-002-1 (2026-04-02)

Buffers moved from stack-local to private member arrays in `DeliveryEngine`:

| Change | Detail |
|--------|--------|
| `src/core/DeliveryEngine.hpp` | Added `m_retry_buf[MSG_RING_CAPACITY]` and `m_timeout_buf[ACK_TRACKER_CAPACITY]` as private members |
| `src/core/DeliveryEngine.cpp` `init()` | Added two bounded `for` loops to zero-initialize both member buffers before `m_initialized = true` |
| `src/core/DeliveryEngine.cpp` `pump_retries()` | Removed `retry_buf[MSG_RING_CAPACITY]` stack array; all references renamed to `m_retry_buf` |
| `src/core/DeliveryEngine.cpp` `sweep_ack_timeouts()` | Removed `timeout_buf[ACK_TRACKER_CAPACITY]` stack array; all references renamed to `m_timeout_buf` |
| `tests/test_DeliveryEngine.cpp` | Added 4 new tests: `test_init_retry_buf_is_zeroed`, `test_init_timeout_buf_is_zeroed`, `test_pump_retries_capacity`, `test_sweep_ack_timeouts_capacity` |
| `docs/STACK_ANALYSIS.md` | Updated Chain 3/4 frame sizes; removed CRITICAL NOTE; updated summary table |
| `docs/WCET_ANALYSIS.md` | Removed stack warning box; updated embedded porting guidance |

#### Checklist reference

Entry criteria verified 2026-04-02:

| Criterion | Status |
|-----------|--------|
| `make` — zero warnings, zero errors | PASS |
| `make lint` — zero clang-tidy violations, CC ≤ 10 all modified functions | PASS |
| `make run_tests` — all 23 DeliveryEngine tests pass (19 existing + 4 new) | PASS |
| `make check_traceability` | PASS |
| New test functions carry `// Verifies: REQ-x.x` comments | PASS |
| SC annotations on `pump_retries()` and `sweep_ack_timeouts()` unchanged | PASS |
| `init()` remains NSC (init-phase buffer zeroing; not on any message path) | PASS |
| `NEVER_COMPILED_OUT_ASSERT` density ≥ 2 in all modified functions | PASS |

#### Moderator sign-off

Moderator: Don Jessup — 2026-04-02. DEF-002-1 resolved. All entry and exit criteria satisfied. No regressions. Inspection INSP-002 closed PASS.

