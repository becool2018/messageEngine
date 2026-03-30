---
name: find_missing_use_cases
description: Compare the live source code against existing UC_*.md files and HIGH_LEVEL_USE_CASES.md; create new UC documents for any capability not yet documented, and update HIGH_LEVEL_USE_CASES.md to include them. Never modifies existing UC files.
user-invocable: true
allowed-tools: Glob, Read, Write
---

Identify capabilities present in the source code that have no corresponding UC
document, then write a new `UC_XX_*.md` for each gap and insert the new UC
entries into `docs/use_cases/HIGH_LEVEL_USE_CASES.md`.

---

## Phase 1 — Read all inputs before doing any analysis

Read the following in parallel:

### 1.1 Format and index

- `docs/use_cases/use_case_format.txt` — the 15-section output format that
  every UC document must follow exactly
- `docs/use_cases/HIGH_LEVEL_USE_CASES.md` — canonical UC list; provides the
  HL group taxonomy, existing UC numbers, and the classification of each entry
  (HL group, Application Workflow, or System Internal)

### 1.2 Discover existing UC documents

Glob `docs/use_cases/UC_*.md` and read every file found.

Build an inventory:
- All UC numbers that already exist (e.g., {01, 02, 03, ...})
- The highest UC number in use (to determine the next free number)
- For each existing UC: its one-line description and HL group affiliation

### 1.3 Read all source files

Glob and read every file in these groups:

**Public API headers:**

    src/**/*.hpp

**Implementation files:**

    src/**/*.cpp

**Test files:**

    tests/**/*.cpp

**App entry points:**

    src/app/*.cpp

---

## Phase 2 — Build the "documented capability" set

From Phase 1, produce two lists:

**Documented capabilities** — every distinct behaviour, function, or scenario
that is already described in at least one existing `UC_*.md` file.  Include:
- Every public API call that has its own UC
- Every internal sub-function listed as a System Internal UC
- Every error path, edge case, or test scenario that already appears in a UC

**Code capabilities** — every distinct behaviour discoverable from the source:
- Every public function/method in `src/**/*.hpp`
- Every internal state transition, sub-flow, or error path visible in
  `src/**/*.cpp`
- Every named test case in `tests/**/*.cpp` that exercises a distinct scenario
  not obviously covered by an existing UC

---

## Phase 3 — Gap analysis

Compare the two lists. A **gap** is a code capability that does not appear in
any existing UC document.

For each gap, apply the classification algorithm below to determine where it
belongs in the taxonomy.

### Classification algorithm

**Actor model:**
- **User** — the application or developer calling the public API
- **System** — messageEngine (treated as a grey box)

**Step A — Is it called directly in `src/app/*.cpp`?**

Yes → it is user-facing. Go to Step B.
No  → it is not called directly by the user. Go to Step C.

**Step B — Does it represent a single API call or configuration action?**

Yes → classify as **HL group** (new or existing) with a new UC entry.
No, it combines two or more calls into a multi-step pattern → classify as
**Application Workflow**.

**Step C — Is it called only from within other `src/` files?**

Yes → classify as **System Internal**.

**Common mistakes to avoid:**
- `public` in C++ does NOT imply user-facing. Apply the algorithm, not visibility.
- A test that calls an internal function directly does NOT make it user-facing.
- Only create a new HL group when the gap genuinely represents a new user goal
  not covered by any existing HL group.

---

## Phase 4 — Assign UC numbers and write new UC documents

For each identified gap (in classification order: HL groups first, Application
Workflow second, System Internals last):

1. Assign the next free UC number (one higher than the current maximum in use;
   increment for each new UC written in this run).

2. Determine the output filename:
   - `docs/use_cases/UC_XX_<slug>.md`
   - `<slug>` = description converted to `lower_snake_case`, stripped of
     punctuation, spaces collapsed to underscores, max 40 characters.

3. Write a complete UC document using the **exact 15-section structure** from
   `use_case_format.txt`.  Requirements for each section:

### Document header (above Section 1)

```
# UC_XX — <one-line description>

**HL Group:** HL-N — <HL group title> | Application Workflow | System Internal
**Actor:** User | System
**Requirement traceability:** <REQ-x.x IDs from Implements:/Verifies: comments
                               in the source files most relevant to this UC>
```

### Section 1 — Use Case Overview
- **Trigger:** exact function call, event, or configuration action; name function and file.
- **Goal:** one sentence — what User (or calling component) wants.
- **Success outcome:** what System returns or does on the OK path.
- **Error outcomes:** every non-OK Result code and the condition producing it.

For System Internal UCs: additionally state which higher-level UC invokes this
sub-function and why it is factored out.

### Section 2 — Entry Points
Name every function the User (or calling component) invokes to start this flow.
Give the full C++ signature and file path.

### Section 3 — End-to-End Control Flow
Numbered, ordered sequence of every meaningful execution step from entry to
return.  Be explicit about:
- Each function call: caller, callee, file, what is passed
- State transitions (e.g. `EntryState::FREE → PENDING` in `AckTracker`)
- Conditional branches that change the outcome
- Exact point where each result code is produced

Do NOT skip steps or write "…does X internally."  Trace every hop.

### Section 4 — Call Tree
ASCII call tree for the primary success path matching the style in
`use_case_format.txt`.

### Section 5 — Key Components Involved
One bullet per major class or free function: responsibility in this UC and why
it must participate.

### Section 6 — Branching Logic / Decision Points
One entry per condition that changes the outcome: condition expression, true-
branch result, false-branch result, where control goes next.

### Section 7 — Concurrency / Threading Behavior
- Thread context
- `std::atomic` loads/stores: variable name and memory order
- Multi-thread safety
- SPSC contracts (`RingBuffer` producer/consumer roles)

### Section 8 — Memory & Ownership Semantics
- Stack buffers: name and size
- Fixed-capacity static arrays: name, capacity constant, source file
- Confirm no heap allocation on this path (Power of 10 Rule 3)
- Lifetime of key objects

### Section 9 — Error Handling Flow
For each non-OK result code: triggering condition, consistency of system state,
expected caller action.

### Section 10 — External Interactions
POSIX/OS API calls, socket reads/writes, signal-flag accesses.  If none, write
one sentence saying the flow operates entirely in process memory.

### Section 11 — State Changes / Side Effects
Every piece of state this flow modifies: owning object, member name, before/after
value, visibility to other components.

### Section 12 — Sequence Diagram
Plain-text (non-Mermaid) diagram in the style from `use_case_format.txt`.
Show at minimum: User → entry point → all inter-component calls → return to User.

### Section 13 — Initialization vs Runtime Flow
Preconditions (what must be initialized) and runtime behavior (steady-state
operation).

### Section 14 — Known Risks / Observations
Race conditions, capacity limits, tight coupling, anything that was inferred
rather than read from source (prefix with `[ASSUMPTION]`).

### Section 15 — Unknowns / Assumptions
Explicit list of inferences not read directly from source.  Prefix every item
with `[ASSUMPTION]`.

---

## Phase 5 — Update HIGH_LEVEL_USE_CASES.md

After writing all new UC documents, update `docs/use_cases/HIGH_LEVEL_USE_CASES.md`:

**For each new UC classified as HL group:**
- If it belongs to an existing HL group: add a new `- UC_XX — <description>`
  line inside that group's bullet list, after the last existing entry for that
  group.
- If it represents a genuinely new user goal: append a new `## HL-N: <Title>`
  section (with `>` summary and bullet list) at the end of the HL groups block,
  before the "Application Workflow" section.  Assign the next sequential HL
  number.

**For each new UC classified as Application Workflow:**
- Add a `- UC_XX — <description> — <one-line rationale>` line inside the
  "Application Workflow" section.

**For each new UC classified as System Internal:**
- Add a `- UC_XX — <description> — <one-line rationale>` line inside the
  "System Internals" section.

**Do not modify any existing line** in `HIGH_LEVEL_USE_CASES.md` other than
inserting new lines in the locations described above.

---

## Phase 6 — Quality checks and report

After writing all new UC documents and updating the index:

1. **No existing UC touched** — confirm that every file written was a new path,
   not an existing `UC_*.md` file.

2. **No empty sections** — every section in every new UC document has
   substantive content derived from source.  If a section truly does not apply,
   write one explicit sentence explaining why.

3. **All assumptions flagged** — anything inferred rather than read from source
   carries the `[ASSUMPTION]` prefix in Section 15.

4. **Correct paths** — all new files are written under `docs/use_cases/`.

5. **HIGH_LEVEL_USE_CASES.md updated** — every new UC appears in the index.

6. **Summary report** — print a table:

   | UC    | Filename                          | HL Group / Category | Gap reason                          |
   |-------|-----------------------------------|---------------------|-------------------------------------|
   | UC_36 | UC_36_delivery_engine_init.md     | HL-16               | DeliveryEngine::init() not documented |
   | …     | …                                 | …                   | …                                   |

   If no gaps are found, print:
   > No gaps found — every code capability has a corresponding UC document.
