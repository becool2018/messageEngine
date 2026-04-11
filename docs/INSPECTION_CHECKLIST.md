# messageEngine — Code Inspection Checklist

Policy: CLAUDE.md §12 / NPR 7150.2D / NASA-STD-8739.8

Complete one copy of this checklist per inspection. File the completed copy in
docs/DEFECT_LOG.md as an attachment reference or inline summary.

---

## Header

| Field         | Value |
|---------------|-------|
| Date          |       |
| Change / PR   |       |
| Author        |       |
| Moderator     |       |
| Reviewer(s)   |       |
| Files reviewed|       |

---

## Part A — Entry gate (verify before starting; abort review if any item fails)

All nine criteria below are required by CLAUDE.md §12.2. No review may begin until
every item is checked PASS.

| # | Item | Pass / Fail |
|---|------|-------------|
| A1 | `make` completes with zero warnings and zero errors | |
| A2 | `make lint` — zero clang-tidy violations (cyclomatic complexity ≤ 10 enforced) | |
| A3 | `make run_tests` — all tests green | |
| A4 | `make check_traceability` — RESULT: PASS | |
| A5 | All new/modified `src/` files carry `// Implements: REQ-x.x` tags in file header | |
| A6 | All new/modified `tests/` files carry `// Verifies: REQ-x.x` tags at file level | |
| A7 | No raw `assert()` in `src/` — `NEVER_COMPILED_OUT_ASSERT` used throughout | |
| A8 | No dynamic allocation on critical paths after init (Power of 10 Rule 3) | |
| A9 | Author has self-reviewed against this checklist before requesting moderator-led review | |

---

## Part B — Traceability (CLAUDE.md §11)

| # | Item | Pass / Fail / N/A |
|---|------|-------------------|
| B1 | Every new/modified `src/` file has `// Implements: REQ-x.x` in its file header | |
| B2 | Every new/modified `tests/` file has `// Verifies: REQ-x.x` at file level | |
| B3 | Every new/modified test function has `// Verifies: REQ-x.x` immediately above it | |
| B4 | No new functionality introduced without a REQ ID assigned in CLAUDE.md first | |
| B5 | If a new REQ ID was added to CLAUDE.md, TRACEABILITY_MATRIX.md was regenerated | |

---

## Part C — Power of 10 compliance (.claude/CLAUDE.md §2)

| # | Rule | Item | Pass / Fail / Deviation |
|---|------|------|-------------------------|
| C1  | Rule 1 | No `goto`, no `setjmp`/`longjmp` | |
| C2  | Rule 1 | No direct or indirect recursion | |
| C3  | Rule 2 | All loops have a statically provable bound, OR carry the Rule 2 deviation comment for designated infrastructure loops | |
| C4  | Rule 3 | No `malloc`/`new` on critical paths after init phase | |
| C5  | Rule 3 | No STL containers/algorithms in `src/` (except `std::atomic`) | |
| C6  | Rule 4 | All functions fit on one printed page | |
| C7  | Rule 4 | Cyclomatic complexity ≤ 10 per function | |
| C8  | Rule 5 | No raw `assert()` in `src/` — `NEVER_COMPILED_OUT_ASSERT` used instead | |
| C9  | Rule 5 | ≥ 2 `NEVER_COMPILED_OUT_ASSERT` calls per non-trivial function | |
| C9a | Rule 5 | Each assertion is independently falsifiable — not logically implied by any prior guard, early return, or variable definition in the same scope. Tautology test: ask "can this condition be false given what has already executed to reach this point?" Common failure modes: (1) `!X \|\| condition_that_is_a_conjunct_of_X` — always true by De Morgan; (2) asserting a value unconditionally set on the immediately preceding line with no intervening branching code; (3) placing `ASSERT(m_flag)` immediately after `if (!m_flag) return;`. When no two independent runtime-falsifiable assertions exist for a small helper (e.g., a single-snapshot conditional log), place the second assertion at the call site using context not available inside the helper, or document the limitation as a named ceiling in `docs/COVERAGE_CEILINGS.md`. | |
| C10 | Rule 6 | Variables declared in the smallest possible scope | |
| C11 | Rule 7 | Every non-void return value is explicitly checked by the caller | |
| C12 | Rule 8 | No macros used for control flow or to obscure structure | |
| C13 | Rule 9 | No explicit function pointer declarations in `src/` | |
| C14 | Rule 9 | Virtual functions conform to MISRA C++:2023 rules on virtuals | |
| C15 | Rule 10 | Zero compiler warnings in this change (enforced by `make lint` — entry gate A2) | |

---

## Part D — MISRA C++:2023 / F-Prime subset (.claude/CLAUDE.md §3–4)

| # | Item | Pass / Fail / Deviation |
|---|------|-------------------------|
| D1 | No exceptions thrown or caught (`-fno-exceptions`) | |
| D2 | No templates in production `src/` code | |
| D3 | No `dynamic_cast`, `typeid`, or RTTI (`-fno-rtti`) | |
| D4 | No C-style casts — `static_cast` / `reinterpret_cast` / `const_cast` only | |
| D5 | C++17 features only; no C++20 or later | |
| D6 | `std::atomic` used for lock-free shared state; no GCC `__atomic` built-ins | |
| D7 | All deviations from MISRA required rules are commented at point of use | |

---

## Part E — Architecture and layering (.claude/CLAUDE.md §5 / CLAUDE.md §2)

| # | Item | Pass / Fail / N/A |
|---|------|-------------------|
| E1 | `src/core/` — no direct OS, socket, or thread API calls | |
| E2 | `src/platform/` — no application-specific policy decisions | |
| E3 | `src/app/` — accesses `src/core` and `src/platform` only through public interfaces | |
| E4 | No cyclic dependencies introduced between modules or directories | |
| E5 | Non-portable code confined to `src/platform/` with clear abstraction boundary | |

---

## Part F — Error handling and robustness (CLAUDE.md §6–7)

| # | Item | Pass / Fail / N/A |
|---|------|-------------------|
| F1 | All external inputs (network, config) validated before use | |
| F2 | Errors classified as WARNING_LO / WARNING_HI / FATAL appropriately | |
| F3 | No unbounded buffer growth — explicit size limits enforced | |
| F4 | No full payload contents logged — metadata and IDs only | |
| F5 | Connection establishment, teardown, and FATAL events logged | |

---

## Part G — Security (.claude/CLAUDE.md §7)

| # | Item | Pass / Fail / N/A |
|---|------|-------------------|
| G1 | No constructs that invoke undefined or implementation-defined behavior | |
| G2 | Safe defaults used — encryption extension points preserved, not removed | |
| G3 | No sensitive data logged | |

---

## Part H — General code quality

| # | Item | Pass / Fail / N/A |
|---|------|-------------------|
| H1 | Naming is consistent with the existing codebase (no mixed conventions) | |
| H2 | Any unavoidable deviation from a rule is commented with the rule reference | |
| H3 | No dead or unreachable code introduced | |
| H4 | No TODO or FIXME left untracked (must have a DEFECT_LOG entry or REQ reference) | |

---

## Part I — Exit gate (moderator completes after all defects dispositioned)

| # | Item | Pass / Fail |
|---|------|-------------|
| I1 | All CRITICAL and MAJOR defects in DEFECT_LOG.md are dispositioned (FIX or WAIVE); MINOR defects may remain DEFER if accompanied by a tracked note | |
| I2 | `make`, `make run_tests`, `make check_traceability` all pass after fixes | |
| I3 | All checklist items above are filled in (no blanks) | |

---

## Moderator sign-off

| Field | Value |
|-------|-------|
| Outcome | PASS / FAIL / CONDITIONAL PASS |
| Conditions (if conditional) | |
| Moderator name / alias | |
| Date | |

---

*Do not edit this template. Copy it for each inspection and record the completed copy
in docs/DEFECT_LOG.md.*
