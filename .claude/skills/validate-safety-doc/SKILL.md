---
name: validate-safety-doc
description: Validate one Safety & Assurance document in docs/ against the live source code. Checks that all claims, constants, function names, state machines, HAZ IDs, REQ IDs, and structural references are still accurate. Focuses on one document per invocation.
user-invocable: true
allowed-tools: Read, Glob, Grep, Bash
---

Validate one Safety & Assurance document from `docs/` against the live source code and
report every discrepancy found.

**Target document** — use the argument passed by the user (e.g., `HAZARD_ANALYSIS`,
`STATE_MACHINES`, `TRACEABILITY_MATRIX`, `STACK_ANALYSIS`, `WCET_ANALYSIS`,
`MCDC_ANALYSIS`, `INSPECTION_CHECKLIST`, `DEFECT_LOG`, `VERIFICATION_POLICY`).

If no argument was given, list the nine documents below and ask the user to pick one
before proceeding:

| # | Document | What it covers |
|---|---|---|
| 1 | `docs/HAZARD_ANALYSIS.md` | Hazard analysis, FMEA, SC/NSC function classification |
| 2 | `docs/STATE_MACHINES.md` | Formal state-transition tables for AckTracker, RetryManager, ImpairmentEngine |
| 3 | `docs/TRACEABILITY_MATRIX.md` | Bidirectional REQ-ID → src/tests traceability matrix |
| 4 | `docs/STACK_ANALYSIS.md` | Worst-case stack depth and frame counts across call chains |
| 5 | `docs/WCET_ANALYSIS.md` | Worst-case execution time (operation counts) for SC functions |
| 6 | `docs/MCDC_ANALYSIS.md` | MC/DC coverage analysis for the five highest-hazard SC functions |
| 7 | `docs/INSPECTION_CHECKLIST.md` | Formal inspection entry/exit criteria and moderator checklist |
| 8 | `docs/DEFECT_LOG.md` | Cumulative inspection defect record |
| 9 | `docs/VERIFICATION_POLICY.md` | VVP-001 verification methods M1–M7 by classification level |

---

## Phase 1 — Read the target document

Read the full content of the selected document. Extract every claim, constant, function
name, state name, REQ ID, HAZ ID, make target, or file path that appears in it.
Call this the **document inventory**.

---

## Phase 2 — Read the relevant live sources

Read the sources specific to the selected document (defined below). Build a
**source inventory** of the same categories of items found in Phase 1.

### HAZARD_ANALYSIS.md — sources to read

- All `src/**/*.hpp` — look for `// Safety-critical (SC): HAZ-NNN` annotations on
  every public function declaration.
- All `src/**/*.hpp` — build a list of every public function declared.
- `CLAUDE.md` — extract every `[REQ-x.x]` ID.
- `docs/HAZARD_ANALYSIS.md` (already read) — extract every HAZ ID, FMEA row, and
  SC/NSC classification entry.

**What to check:**
1. Every function listed as SC in the document has a `// Safety-critical (SC): HAZ-NNN`
   comment on its `.hpp` declaration (exact match of function name and HAZ IDs).
2. Every function listed as NSC does NOT have an SC annotation.
3. Every public function in `src/**/*.hpp` appears in the SC or NSC classification table
   (no unclassified public functions).
4. Every HAZ ID referenced in an FMEA row has a corresponding HAZ-NNN entry in the
   Hazard Analysis section.
5. No HAZ ID is referenced in the document but absent from the Hazard Analysis section.

### STATE_MACHINES.md — sources to read

- `src/core/AckTracker.hpp` and `src/core/AckTracker.cpp`
- `src/core/RetryManager.hpp` and `src/core/RetryManager.cpp`
- `src/platform/ImpairmentEngine.hpp` and `src/platform/ImpairmentEngine.cpp`

**What to check:**
1. Every state name in the document matches an `enum` value or named constant in the code.
2. Every transition in the state table (State A → Event → State B) has a corresponding
   code path (conditional branch or assignment) in the implementation.
3. Every guard condition listed matches the actual `if`/`else` condition in code.
4. Every invariant claimed in the document is enforced by a `NEVER_COMPILED_OUT_ASSERT`
   or a structural constraint in the code.
5. No state, transition, or invariant exists in the code that is absent from the document.

### TRACEABILITY_MATRIX.md — sources to read

- All `src/**/*.cpp` — search for `// Implements: REQ-` comments; extract each REQ ID
  and the file it appears in.
- All `tests/**/*.cpp` — search for `// Verifies: REQ-` comments; extract each REQ ID
  and the file it appears in.
- `CLAUDE.md` — extract every `[REQ-x.x]` ID defined in §§3–7.

**What to check:**
1. Every `[REQ-x.x]` defined in `CLAUDE.md` appears in the matrix.
2. Every REQ ID in the matrix has at least one `// Implements:` entry in `src/`.
3. Every REQ ID in the matrix has at least one `// Verifies:` entry in `tests/`.
4. No REQ ID appears in `src/` or `tests/` that is absent from the matrix.
5. The file paths in the matrix match actual file paths in the repo.

### STACK_ANALYSIS.md — sources to read

- All `src/**/*.cpp` and `src/**/*.hpp` — look for stack-allocated buffers (fixed-size
  `char`, `uint8_t`, or struct arrays declared on the stack).
- Identify every call chain rooted at a public API function; count frames and sum
  the stack-allocated buffer sizes per chain.
- `src/core/Types.hpp` — verify capacity constants referenced in the document.

**What to check:**
1. Every stack-allocated buffer cited in the document exists at the stated line/function
   and has the stated size.
2. The call chain frame counts and depths match the actual call graph.
3. The worst-case chain and byte count match what the code currently produces.
4. If any function added a new buffer >256 bytes or the call chain changed, flag it.
5. All capacity constants referenced match current values in `Types.hpp`.

### WCET_ANALYSIS.md — sources to read

- `src/core/Types.hpp` — extract all capacity constants
  (`ACK_TRACKER_CAPACITY`, `DEDUP_WINDOW_SIZE`, `MSG_MAX_PAYLOAD_BYTES`,
  `RETRY_MANAGER_CAPACITY`, `IMPAIR_DELAY_BUF_SIZE`, `MAX_TCP_CONNECTIONS`, etc.).
- `src/core/DeliveryEngine.cpp`, `src/core/AckTracker.cpp`,
  `src/core/RetryManager.cpp`, `src/core/DuplicateFilter.cpp`,
  `src/core/Serializer.cpp` — verify that the loop structures and dominant operation
  counts in the document match the actual algorithmic structure.

**What to check:**
1. Every constant named in the document matches its current value in `Types.hpp`.
2. The operation count formula for each SC function matches the current loop structure
   (e.g., O(N) over a capacity constant — verify the loop bound and inner work).
3. The dominant path claimed for each function is still the actual dominant path.
4. Any new SC function added since the document was written is flagged as missing.

### MCDC_ANALYSIS.md — sources to read

- `src/core/DeliveryEngine.hpp` and `src/core/DeliveryEngine.cpp`
- `src/core/DuplicateFilter.hpp` and `src/core/DuplicateFilter.cpp`
- `src/core/Serializer.hpp` and `src/core/Serializer.cpp`
- `docs/HAZARD_ANALYSIS.md` — verify the five functions listed are still the top five
  highest-hazard SC functions.
- `tests/test_DeliveryEngine.cpp`, `tests/test_DuplicateFilter.cpp`,
  `tests/test_Serializer.cpp` — verify each test case cited in the document exists.

**What to check:**
1. The five functions analysed are still classified as the top five highest-hazard SC
   functions in `HAZARD_ANALYSIS.md`.
2. Every decision point (condition) listed in the document exists in the current code
   at the stated location; note any that have been added or removed.
3. Every test case name referenced as providing MC/DC coverage exists in the test file.
4. The MC/DC argument for each condition (which test provides the independence pair)
   is still valid given the current test suite.

### INSPECTION_CHECKLIST.md — sources to read

- `Makefile` — verify every `make <target>` referenced in the checklist exists.
- `CLAUDE.md` — verify that entry/exit criteria reference the correct REQ/file structure.
- `docs/DEFECT_LOG.md` — verify the defect severity table matches what the checklist
  defines.

**What to check:**
1. Every `make <target>` named in the checklist exists in the `Makefile`.
2. All file paths referenced (e.g., `docs/DEFECT_LOG.md`, `CLAUDE.md`) are correct.
3. The severity levels defined match those used in `DEFECT_LOG.md`.
4. No make target has been renamed or removed since the checklist was written.

### DEFECT_LOG.md — sources to read

- Run `git log --oneline -30` — verify that any commit hash or PR reference cited in
  the log still exists.
- Read every source file referenced in a defect entry — verify the file still exists
  and the line numbers / function names cited are still present.

**What to check:**
1. Every source file referenced in a defect entry exists in the repo.
2. Every function name cited in a defect entry still exists in that file
   (flag if the function was renamed or removed).
3. Every "Disposition" marked "Fixed" references a commit or PR that resolves the defect.
4. No open defect (`Status: OPEN`) exists for an SC function without a stated assignee
   and target date.

### VERIFICATION_POLICY.md — sources to read

- `Makefile` — verify every make target named in the policy (`make coverage`,
  `make lint`, `make run_tests`, `make cppcheck`, `make pclint`) exists or is
  explicitly listed as PENDING.
- `.github/workflows/CI.yml` (if present) — verify that CI steps match the M1–M7
  methods described.
- `CLAUDE.md` §17 — verify the software classification and verification baseline
  described in the policy still match.

**What to check:**
1. Every make target named in the policy exists in the `Makefile` or is flagged PENDING
   with justification.
2. The software classification (Class C) and voluntary Class B rigor statement still
   match `CLAUDE.md` §17.
3. The M1–M7 method descriptions match the actual tools and processes in use.
4. Any new verification step added to CI that is not described in the policy is flagged.

---

## Phase 3 — Produce the validation report

Structure the report as follows:

### Summary

One-paragraph summary: document name, date validated, overall status
(PASS / PASS-WITH-WARNINGS / NEEDS-UPDATE), and the count of findings by severity.

### Findings

For each discrepancy found, emit one row:

| # | Severity | Location in doc | Claim / expected | Actual in source | Action required |
|---|---|---|---|---|---|

Severity levels:
- **CRITICAL** — a claim is factually wrong (wrong constant value, missing SC annotation,
  state that does not exist in code, broken make target).
- **STALE** — a claim was correct when written but the code has since changed
  (function renamed, constant updated, test case removed).
- **MISSING** — something exists in the source that the document does not cover
  (new public function not classified, new REQ ID not in matrix, new SC function
  not in WCET analysis).
- **MINOR** — a cosmetic or non-normative inconsistency (wrong line number reference,
  typo in a function name that is otherwise correct).

### Items confirmed accurate

Bullet list of the major claims that were verified correct — gives confidence that the
check was thorough, not just a list of failures.

### Recommended actions

Numbered list of concrete edits needed to bring the document up to date, in priority
order (CRITICAL first, then STALE, then MISSING, then MINOR).

---

## Phase 4 — Offer to apply fixes

After presenting the report, ask the user:

> Would you like me to apply the recommended fixes to `docs/<DOCUMENT>.md` now?

If yes: make only the changes identified in the Findings table. Do not rewrite sections
that were confirmed accurate. Do not add new content beyond what is needed to resolve
the specific findings.

If no: stop. The report stands as the deliverable.
