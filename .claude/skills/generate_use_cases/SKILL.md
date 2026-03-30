---
name: generate_use_cases
description: Read HIGH_LEVEL_USE_CASES.md and the live source code, then regenerate (or create) every individual UC_*.md document in docs/use_cases/ following the use_case_format.txt flow-of-control format. Overwrites existing UC files; creates new ones for any UC number without an existing file.
user-invocable: true
allowed-tools: Glob, Read, Write
---

Regenerate every individual use case document in `docs/use_cases/` by tracing
the live source code and following the detailed flow-of-control format defined
in `docs/use_cases/use_case_format.txt`.

One `.md` file is written per UC entry found in `HIGH_LEVEL_USE_CASES.md`
(HL groups, Application Workflow, and System Internals sections all included).
Existing files are **overwritten**; new UC numbers get newly created files.

---

## Phase 1 — Read all inputs before writing anything

Read the following in parallel before writing a single document:

### 1.1 Format and index

- `docs/use_cases/use_case_format.txt` — the 15-section output format every
  UC document must follow exactly
- `docs/use_cases/HIGH_LEVEL_USE_CASES.md` — canonical UC list; each entry
  provides: UC number, one-line description, and HL group context

### 1.2 Discover existing UC filenames

Glob `docs/use_cases/UC_*.md`.

For each UC number that appears in `HIGH_LEVEL_USE_CASES.md`:
- If a file whose name starts with that UC number already exists (e.g.
  `UC_01_best_effort_send.md`), write to that exact path (overwrite).
- If no matching file exists, create a new file at:
  `docs/use_cases/UC_XX_<slug>.md`
  where `<slug>` is the UC description converted to `lower_snake_case`
  (strip punctuation, collapse spaces to underscores, max 40 characters).

### 1.3 Read all source files

Glob and read every file in these groups. All must be read before writing
begins so the complete call graph is available for every UC.

**Public API headers:**

    src/**/*.hpp

**Implementation files (state machines, internal logic, error paths):**

    src/**/*.cpp

**Test files (concrete scenarios, edge cases, boundary conditions):**

    tests/**/*.cpp

---

## Phase 2 — Write each UC document

Iterate over every UC entry in `HIGH_LEVEL_USE_CASES.md` in document order
(HL groups first, Application Workflow second, System Internals last).

For each UC write a complete document using the **exact 15-section structure**
from `use_case_format.txt`. The detailed requirements for each section follow.

---

### Document header (above Section 1)

```
# UC_XX — <description from HIGH_LEVEL_USE_CASES.md>

**HL Group:** HL-N — <HL group title>
**Actor:** User | System
**Requirement traceability:** <REQ-x.x IDs from Implements:/Verifies: comments
                               in the source files most relevant to this UC>
```

---

### Section 1 — Use Case Overview

Provide:
- **Trigger:** The exact function call, event, or configuration action that
  starts this flow. Name the function and its file.
- **Goal:** One sentence — what the User (or calling component) wants to achieve.
- **Success outcome:** What the System returns or does when the flow completes
  without error (include the `Result::OK` path and any output parameters).
- **Error outcomes:** Every non-OK `Result` code this flow can return and the
  condition that causes each one.

For **System Internal** UCs, additionally state which higher-level UC invokes
this sub-function and why it is factored out as a distinct mechanism.

---

### Section 2 — Entry Points

Name every function the User (or calling component) invokes to start this flow.
Give the full C++ signature and the file path. For System Internals, name the
calling function and the file it lives in.

---

### Section 3 — End-to-End Control Flow

Provide a numbered, ordered sequence of every meaningful execution step from
entry to return. Be explicit about:
- Each function call: caller, callee, file, and what is passed
- State transitions (e.g. `EntryState::FREE → PENDING` in `AckTracker`)
- Conditional branches that change the outcome (name the condition)
- The exact point where each result code is produced and returned

Do NOT skip steps or write "…does X internally." Trace every hop.

---

### Section 4 — Call Tree

ASCII call tree for the primary success path, matching the style from
`use_case_format.txt`. Include only calls relevant to this UC's flow.

---

### Section 5 — Key Components Involved

One bullet per major class or free function in the flow:
- Its responsibility within this specific UC
- Why it must participate (what would break without it)

---

### Section 6 — Branching Logic / Decision Points

One entry per condition that changes the outcome:
- Condition expression (as close to source text as possible)
- True-branch result
- False-branch result
- Where control goes next in each case

---

### Section 7 — Concurrency / Threading Behavior

State:
- Which thread context executes this flow
- Any `std::atomic` loads or stores on the path (name the variable and the
  memory order used — acquire/release/relaxed)
- Whether this call is safe to issue from multiple threads simultaneously
- Any SPSC contracts (e.g. `RingBuffer` producer/consumer roles) relied on

---

### Section 8 — Memory & Ownership Semantics

State:
- Stack buffers used: name, size in bytes
- Fixed-capacity static arrays accessed: name, capacity constant, source file
- Confirm `Power of 10 Rule 3`: no heap allocation occurs on this path
  (or document any init-phase allocation explicitly)
- Lifetime of key objects: who creates them, who destroys them, when

---

### Section 9 — Error Handling Flow

For each non-OK result code this flow can produce:
- Which condition triggers it
- Whether System state is left consistent after the error
- What the caller is expected to do (retry, log, propagate, abort)

---

### Section 10 — External Interactions

List every interaction with:
- POSIX / OS APIs: `clock_gettime`, `send`, `recv`, `poll`, `accept`, etc.
  (include the socket fd or clock type used)
- Network sockets: file descriptor read or written and direction
- Signal handlers: if this flow reads or sets `g_stop_flag` or similar

If none, write: "None — this flow operates entirely in process memory."

---

### Section 11 — State Changes / Side Effects

List every piece of state this flow modifies:
- Owning object and member name
- Value before and after (or characterize the transition: FREE→PENDING, etc.)
- Whether the change is visible to future calls by other components

---

### Section 12 — Sequence Diagram

Plain-text (non-Mermaid) sequence diagram in the style from
`use_case_format.txt`. Show at minimum:
- User calling the entry point
- All inter-component calls
- Return path back to User

---

### Section 13 — Initialization vs Runtime Flow

Distinguish:
- Preconditions: what must already be initialized before this flow runs
- Runtime behavior: what this flow does during steady-state operation

---

### Section 14 — Known Risks / Observations

Note any of:
- Race conditions possible if threading contracts are violated
- Capacity limits (`ACK_TRACKER_CAPACITY`, `DEDUP_WINDOW_SIZE`,
  `IMPAIR_DELAY_BUF_SIZE`, `MSG_RING_CAPACITY`) that silently degrade behavior
  when exhausted
- Tight coupling that could make this flow fragile to refactoring
- Anything marked `[ASSUMPTION]` that was inferred rather than read from source

---

### Section 15 — Unknowns / Assumptions

Explicit list of anything inferred rather than read directly from source.
Prefix every item with `[ASSUMPTION]`.

---

## Phase 3 — Source-to-UC focus map

After reading all files in Phase 1, use this table to decide which files to
**focus on** when tracing each UC's flow. The full source set is still
available; this table only guides where to look first.

| UC category (from HL group or description)         | Primary files to trace                                                    |
|----------------------------------------------------|---------------------------------------------------------------------------|
| Send path — HL-1, HL-2, HL-3, HL-4                | `DeliveryEngine.cpp`, `TcpBackend.cpp`, `Serializer.cpp`, `RingBuffer.hpp`|
| Receive path — HL-5                                | `DeliveryEngine.cpp`, `TcpBackend.cpp`, `DuplicateFilter.cpp`, `Timestamp.hpp` |
| Duplicate suppression — HL-6                       | `DuplicateFilter.cpp`, `DeliveryEngine.cpp`                               |
| TCP server / client / teardown — HL-7, HL-8, HL-9 | `TcpBackend.cpp`, `SocketUtils.cpp`                                       |
| Retry pump — HL-10                                 | `DeliveryEngine.cpp`, `RetryManager.cpp`                                  |
| ACK timeout sweep — HL-11                          | `DeliveryEngine.cpp`, `AckTracker.cpp`                                    |
| Impairment configuration — HL-12                  | `ImpairmentEngine.cpp`, `ImpairmentConfig.hpp`                            |
| PRNG seeding — HL-13                               | `PrngEngine.hpp`, `ImpairmentEngine.cpp`                                  |
| Local simulation — HL-14                           | `LocalSimHarness.cpp`, `ImpairmentEngine.cpp`                             |
| Logging / observability — HL-15                    | `Logger.hpp` and every file that calls `Logger::log()`                   |
| Initialization / config — HL-16                    | `DeliveryEngine.cpp`, `TcpBackend.cpp`, `ChannelConfig.hpp`               |
| UDP transport — HL-17                              | `UdpBackend.cpp`, `SocketUtils.cpp`, `Serializer.cpp`                     |
| Application Workflow                               | `src/app/Server.cpp`, `src/app/Client.cpp`, `DeliveryEngine.cpp`          |
| System Internals — serialization                   | `Serializer.cpp`                                                          |
| System Internals — TCP framing / polling           | `TcpBackend.cpp`, `SocketUtils.cpp`                                       |

---

## Phase 4 — Quality checks

After writing all UC documents, verify:

1. **Count matches** — the number of files written equals the number of UC
   entries in `HIGH_LEVEL_USE_CASES.md` (count HL group entries +
   Application Workflow entries + System Internals entries).

2. **No empty sections** — every section has substantive content derived from
   source. If a section does not apply (e.g. "External Interactions" for a
   pure in-memory flow), write one explicit sentence explaining why, not a
   blank line.

3. **All assumptions flagged** — anything inferred rather than read from source
   carries the `[ASSUMPTION]` prefix in Section 15.

4. **Correct paths** — every file is written under `docs/use_cases/`.

5. **Summary report** — when all files are written, print a table:

   | UC    | Filename                        | New or Overwrite |
   |-------|---------------------------------|------------------|
   | UC_01 | UC_01_best_effort_send.md       | Overwrite        |
   | …     | …                               | …                |
