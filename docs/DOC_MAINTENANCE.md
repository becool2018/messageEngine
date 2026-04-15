# Documentation Maintenance Process

**Scope:** All documents in `docs/` and root-level markdown files
**Authority:** CLAUDE.md §§11–17 (traceability, inspection, safety, coverage, WCET, stack)
**Last updated:** 2026-04-15

---

## 1. How to use this guide

Find the kind of change you made in §2 (the trigger table). Each row lists every document
that must be updated, in what order, and whether the update is script-assisted or manual.
Finish by running the CI gate checks in §3 and filing an INSP record per §4 if required.

---

## 2. Trigger table — what you changed → what to update

### 2.1 Capacity constant changed (`src/core/Types.hpp`)

A capacity constant is any of: `ACK_TRACKER_CAPACITY`, `DEDUP_WINDOW_SIZE`,
`MSG_RING_CAPACITY`, `IMPAIR_DELAY_BUF_SIZE`, `MSG_MAX_PAYLOAD_BYTES`, `MAX_RETRY_COUNT`,
`MAX_TCP_CONNECTIONS`, or any new compile-time bound in Types.hpp.

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/CAPACITY_REFERENCE.md` | Update the "Current value" and "Memory impact" for the changed constant. Add a new row if the constant is new. |
| 2 | `docs/WCET_ANALYSIS.md` | Update the O-notation bound and operations count for every SC function whose WCET depends on the changed constant. |
| 3 | `docs/STACK_ANALYSIS.md` | Re-check chain depths if any buffer sized by the constant lives on the stack. Update peak-stack estimates if changed. |
| 4 | `README.md` | Update the "Quick capacity reference" table in the overview section. |
| 5 | Run `make run_stress_tests` | Stress tests are required on capacity constant changes per CLAUDE.md §1c. All suites must pass before merge. |

---

### 2.2 New public function added, or existing public function significantly changed

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/HAZARD_ANALYSIS.md §3` | Classify the function SC or NSC. If SC, assign a HAZ-NNN ID and add a §1 hazard entry and §2 FMEA row. |
| 2 | `src/*.hpp` (declaration) | If SC: add `// Safety-critical (SC): HAZ-NNN` comment on the declaration. |
| 3 | `docs/STATE_MACHINES.md` | If the function drives or queries a state machine, add or update the affected state transition table and invariants. |
| 4 | `docs/MCDC_ANALYSIS.md` | If the function is one of the five highest-hazard SC functions, update the decision table and MC/DC pair listing. |
| 5 | `docs/VERIFICATION_POLICY.md §6.1` | Update the completion status table entry for the relevant component. |
| 6 | `docs/STACK_ANALYSIS.md` | If the function introduces a new call chain or a stack buffer > 256 bytes, add or update the chain entry. |
| 7 | `CLAUDE.md §§3–7` | Assign a new REQ-x.x ID to any new externally-observable behaviour and add it to the requirements list. |
| 8 | Add `// Implements: REQ-x.x` | Add the tag to the implementing `.cpp` file's header block. |
| 9 | Add `// Verifies: REQ-x.x` | Add the tag to every test function that exercises the new behaviour. |
| 10 | Run `make check_traceability` | Must report RESULT: PASS before commit. |
| 11 | Regenerate `docs/TRACEABILITY_MATRIX.md` | `bash docs/check_traceability.sh` — commit the regenerated matrix with the code change. |
| 12 | `docs/DEFECT_LOG.md` | Open an INSP record if the change is non-trivial (see §4). |

---

### 2.3 New SC state machine or change to an existing state machine

Applies to AckTracker, RetryManager, ImpairmentEngine, ReassemblyBuffer, OrderingBuffer,
or any new component classified SC.

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/STATE_MACHINES.md` | Add or update the state transition table, invariants, guard conditions, and entry/exit actions for the changed machine. Use the existing tabular format. |
| 2 | `docs/HAZARD_ANALYSIS.md §1–§2` | Review HAZ entries that reference the state machine; update Mitigation and FMEA rows if the state set, transition set, or error handling changed. |
| 3 | `docs/VERIFICATION_POLICY.md §4` | If new branches are permanently unreachable (NEVER_COMPILED_OUT_ASSERT [[noreturn]] paths), document them as architectural ceilings. |
| 4 | `docs/MCDC_ANALYSIS.md` | Re-analyse MC/DC coverage if any decision inside a highest-hazard SC function changed. |
| 5 | `docs/COVERAGE_CEILINGS.md` | Run `make coverage`; if branch percentages changed, update the affected file row and add a new "Round N update" section. |

---

### 2.4 Wire format change (envelope fields, protocol version, magic)

| Order | Document | Action |
|-------|----------|--------|
| 1 | `src/core/ProtocolVersion.hpp` | Bump `PROTO_VERSION`. `PROTO_MAGIC` is fixed — never change it. |
| 2 | `CLAUDE.md §3.2.8` | Update the version byte description if the layout change is non-trivial. |
| 3 | `docs/Design.md §10` | Update the wire-format section with the new field layout and version byte. |
| 4 | `docs/SECURITY_ASSUMPTIONS.md` | Update or add an assumption entry if the wire change affects source validation, dedup, or ordering guarantees. |
| 5 | Traceability and INSP | Follow steps 8–12 from §2.2. |

---

### 2.5 TLS/DTLS backend change (TlsTcpBackend, DtlsUdpBackend, TlsSessionStore)

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/HAZARD_ANALYSIS.md` | Review HAZ-017 through HAZ-025; update FMEA rows if certificate validation, session resumption, or cookie exchange logic changed. |
| 2 | `docs/SECURITY_ASSUMPTIONS.md §§4–15` | Update any assumption whose enforcement changed (e.g., hostname verification, CRL enforcement, forward secrecy). |
| 3 | `docs/STACK_ANALYSIS.md` | DTLS backends carry large `delayed[]` buffers (~130 KB per frame); re-verify chain depths if buffer sizing changed. |
| 4 | `docs/COVERAGE_CEILINGS.md` | Run `make coverage`; update TlsTcpBackend.cpp and DtlsUdpBackend.cpp rows if branch counts changed. |
| 5 | Traceability and INSP | Follow steps 8–12 from §2.2. |

---

### 2.6 New transport backend or platform adapter added

| Order | Document | Action |
|-------|----------|--------|
| 1 | `CLAUDE.md §§1b, 6` | Add layering rules and REQ IDs for the new backend. |
| 2 | `docs/HAZARD_ANALYSIS.md §§1–3` | Add hazard entries, FMEA rows, and SC classification for every public function. |
| 3 | `docs/SECURITY_ASSUMPTIONS.md` | Add a new section for any trust or contract assumption the new backend relies on. |
| 4 | `docs/STACK_ANALYSIS.md` | Add a new worst-case call chain for the new backend's send and receive paths. |
| 5 | `docs/WCET_ANALYSIS.md` | Add WCET bounds for SC functions in the new backend. |
| 6 | `docs/VERIFICATION_POLICY.md §6.1` | Add a row for the new component and its verification status. |
| 7 | `README.md` | Add the new transport to the feature list. |
| 8 | `docs/Design.md §8` | Add a module design section for the new backend. |
| 9 | `docs/POSSIBLE_APPLICATIONS.md` | Add example deployment scenarios if the new backend enables new use cases. |
| 10 | Traceability, coverage, INSP | Follow steps 8–12 from §2.2; run `make coverage`; add to `docs/COVERAGE_CEILINGS.md`. |

---

### 2.7 Logger / logging infrastructure change

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/LOGGING.md` | Update the affected section: §2 (format), §3 (architecture), §6 (macro reference), §7 (severity levels), §9 (extending), §11 (thread safety). |
| 2 | `README.md §7` | Update the "Observable by default" observability bullet and the link to LOGGING.md if the high-level description changed. |
| 3 | `docs/STACK_ANALYSIS.md` | Logger::log() owns a 512-byte stack buffer; if the buffer size changed, re-verify the call chain depth and peak-stack estimates. |
| 4 | `docs/COVERAGE_CEILINGS.md` | Run `make coverage`; update Logger.cpp, PosixLogClock.cpp, PosixLogSink.cpp rows. |

---

### 2.8 Build system change (Makefile, CMakeLists.txt)

| Order | Document | Action |
|-------|----------|--------|
| 1 | `CONTRIBUTING.md` | Update the "Entry criteria" table if any required `make` target was added, removed, or renamed. |
| 2 | `docs/STATIC_ANALYSIS_TOOLCHAIN.md` | Update tool versions, new flags, or new tier additions. |
| 3 | `README.md` build section | Update build instructions if user-facing commands changed. |
| 4 | `CLAUDE.md §1c` | Update CI/commit rules if the required pre-commit gate changed. |

---

### 2.9 Standards or coding policy change

| Order | Document | Action |
|-------|----------|--------|
| 1 | `.claude/CLAUDE.md` | Update the global standard (version number, new rule, conflict resolution). |
| 2 | `docs/STANDARDS_AND_CONFLICTS.md` | Add the new standard version or record the conflict resolution in §5. |
| 3 | `docs/INSPECTION_CHECKLIST.md` | Update Part C (Power of 10) or Part D (MISRA) if the rule set changed. |
| 4 | `CONTRIBUTING.md` code style table | Reflect any change to required compiler flags, naming, or safety annotations. |

---

### 2.10 Coverage run completed (`make coverage`)

Run `make coverage` before every merge to main (CLAUDE.md §1c).

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/COVERAGE_CEILINGS.md` | Append a new "Round N update" section: date, branch counts per file, new thresholds if any missed branch is now covered, new ceiling justification if a new architecturally-unreachable branch was identified. |
| 2 | `CLAUDE.md §14` | If any per-file threshold changed, update the numeric ceiling in the table. A regression on any SC function file is a blocking defect — open an INSP record. |
| 3 | `docs/VERIFICATION_POLICY.md §6.1` | Update the "M4 last verified" date for each SC function whose coverage was measured. |

---

### 2.11 Formal inspection completed (INSP record)

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/DEFECT_LOG.md` | Append a new INSP-NNN block using the template at the top of the file. Include: date, author, moderator, scope, entry/exit gate verification, defects table (severity, description, disposition), sign-off. |
| 2 | `docs/VERIFICATION_POLICY.md §6.1` | Update the "M1 last completed" date for the component inspected. |
| 3 | `docs/TRACEABILITY_MATRIX.md` | Re-run `docs/check_traceability.sh` after all defect fixes are committed to ensure no tag was removed inadvertently. |

---

### 2.12 Use case (UC) added or behavioural change

| Order | Document | Action |
|-------|----------|--------|
| 1 | `docs/use_cases/UC_NN.md` | Create or update the use case file using `docs/use_cases/use_case_format.txt` as the template. Verify that REQ links are accurate. |
| 2 | `docs/use_cases/HIGH_LEVEL_USE_CASES.md` | Add the new UC to the master list with a one-line description. |
| 3 | `docs/use_cases/USE_CASE_FREQUENCY.md` | Assign or update the frequency and priority classification. |
| 4 | Run `make check_traceability` | Confirm the REQ IDs referenced in the UC are implemented and verified. |

---

## 3. CI gate checks — run before every commit

These are non-optional per CLAUDE.md §1c:

```
make lint              # zero clang-tidy violations
make run_tests         # all tests green
make check_traceability  # RESULT: PASS
```

These are required before merge to main:

```
make coverage          # branch coverage — update COVERAGE_CEILINGS.md if counts changed
```

These are required on capacity constant or algorithm changes:

```
make run_stress_tests  # all stress suites pass
```

---

## 4. When to open a formal INSP record

An INSP record in `docs/DEFECT_LOG.md` is required for every non-trivial change to
`src/` or `tests/`. Use the following threshold:

| Change type | INSP required? |
|-------------|---------------|
| New SC function or state machine | **Yes — mandatory** |
| Change to existing SC function | **Yes — mandatory** |
| New transport backend or major new component | **Yes — mandatory** |
| Bug fix affecting safety-critical path | **Yes — mandatory** |
| Bug fix on NSC path | Yes, unless single-line and trivially correct |
| New test file or test suite | Yes |
| Documentation-only change | No |
| Build / toolchain change | No, unless it changes CI gate behaviour |
| Refactor with no behaviour change, zero SC functions touched | Optional (author discretion) |

INSP records use the checklist in `docs/INSPECTION_CHECKLIST.md`. Entry criteria (all
must be true before review begins) are listed in CLAUDE.md §12.2.

---

## 5. Document ownership and maintenance types

| Document | Maintenance type | Owner |
|----------|-----------------|-------|
| `docs/TRACEABILITY_MATRIX.md` | **Auto-generated** — run `docs/check_traceability.sh` | CI |
| `docs/DEFECT_LOG.md` | **Append-only** — never edit past entries | INSP moderator |
| `docs/COVERAGE_CEILINGS.md` | **Script-assisted** — run `make coverage`, append round section | Coverage engineer |
| `docs/HAZARD_ANALYSIS.md` | Manual — change-controlled safety artifact | Safety engineer |
| `docs/STATE_MACHINES.md` | Manual — formal specification | Core architect |
| `docs/MCDC_ANALYSIS.md` | Manual — updated per decision logic change | MC/DC analyst |
| `docs/VERIFICATION_POLICY.md` | Manual — policy document | Project maintainer |
| `docs/WCET_ANALYSIS.md` | Manual — updated per capacity/algorithm change | Performance analyst |
| `docs/STACK_ANALYSIS.md` | Manual — updated per stack/chain change | Performance analyst |
| `docs/CAPACITY_REFERENCE.md` | Manual — updated per Types.hpp change | Types.hpp maintainer |
| `docs/SECURITY_ASSUMPTIONS.md` | Manual — updated per API contract change | Core/platform architect |
| `docs/INSPECTION_CHECKLIST.md` | Template — updated only when criteria change | Project maintainer |
| `docs/LOGGING.md` | Manual — updated per logger change | Logger maintainer |
| `docs/Design.md` | Manual — updated per architectural change | Chief architect |
| `docs/STATIC_ANALYSIS_TOOLCHAIN.md` | Manual — updated per tool adoption | Build engineer |
| `docs/STANDARDS_AND_CONFLICTS.md` | Manual — updated per standards adoption | Project maintainer |
| `CLAUDE.md` | Manual — primary requirements source | Project maintainer |
| `README.md` | Manual — updated per feature/capacity change | Project maintainer |
| `CONTRIBUTING.md` | Manual — updated per workflow change | Project maintainer |
| `docs/use_cases/UC_*.md` | Manual — updated per feature change | Feature owner |

---

## 6. Documents that are never edited directly

| Document | How it is updated |
|----------|------------------|
| `docs/TRACEABILITY_MATRIX.md` | Run `bash docs/check_traceability.sh` — commit the output |
| `docs/DEFECT_LOG.md` | Append new INSP-NNN blocks only — no edits to closed records |

---

## 7. Quick reference — pre-merge checklist

Copy this block into the PR description when ready for review:

```
## Pre-merge documentation checklist

- [ ] make lint             — PASS
- [ ] make run_tests        — all green
- [ ] make check_traceability — RESULT: PASS
- [ ] make coverage         — COVERAGE_CEILINGS.md updated if counts changed
- [ ] make run_stress_tests — PASS (if capacity constant or algorithm changed)
- [ ] HAZARD_ANALYSIS.md    — reviewed; new SC annotations added if needed
- [ ] STATE_MACHINES.md     — updated if any state machine changed
- [ ] MCDC_ANALYSIS.md      — updated if any highest-hazard decision changed
- [ ] DEFECT_LOG.md         — INSP record filed if non-trivial src/ change
- [ ] TRACEABILITY_MATRIX.md — regenerated (check_traceability.sh)
- [ ] All Implements/Verifies tags present in changed files
- [ ] No raw assert() in src/ — NEVER_COMPILED_OUT_ASSERT used throughout
```
