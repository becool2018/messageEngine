---
name: generate_use_case_list
description: Read the live source code in src/ and regenerate docs/use_cases/HIGH_LEVEL_USE_CASES.md from scratch, keeping the same format.
user-invocable: true
allowed-tools: Glob, Read, Write
---

Regenerate `docs/use_cases/HIGH_LEVEL_USE_CASES.md` by reading the **live source
code** — not the existing UC_*.md files — to discover what the system actually
implements today.

## Steps

### 1. Read the format reference

Read `docs/use_cases/use_case_format.txt` to understand what a "use case"
means in this project (what triggers a flow, expected outcome, actor model).

Read the current `docs/use_cases/HIGH_LEVEL_USE_CASES.md` for format only —
use the HL group structure, heading style, and prose conventions as a template.
Do not treat its content as authoritative; the source code takes precedence.

### 2. Discover and read all source headers

Use Glob to find every `.hpp` file under `src/`:

    src/**/*.hpp

Read every file returned. This automatically picks up new files and ignores
deleted ones — do not rely on any hardcoded filename list.

Focus on extracting from each header:
- All public functions and their signatures
- All enumerations (`ReliabilityClass`, `MessageType`, `Result`, `TransportKind`,
  impairment flags, ordering modes, etc.)
- All configurable behaviours (delivery modes, channel options, impairment types)
- All state machines and their states

### 3. Discover and read all implementation files

Use Glob to find every `.cpp` file under `src/`:

    src/**/*.cpp

Read every file returned. Implementation files reveal:
- Internal state transitions and sub-flows not visible from headers
- Error and fallback paths
- Edge-case behaviours (e.g. retry cancellation on ACK, ACK tracker timeout
  sweep, delay buffer overflow)
- Concurrency and threading details

### 4. Discover and read all test files

Use Glob to find every `.cpp` file under `tests/`:

    tests/**/*.cpp

Read every file returned. Test files are the richest source of specific use
cases — each test function documents a distinct scenario that must work:
- Edge cases (100% packet loss, dedup window eviction, PRNG reproducibility)
- Error injection scenarios
- Boundary conditions and configuration variations

Extract the name and intent of every individual test function as a candidate
detailed use case.

### 5. Discover and read all app entry points

Use Glob to find every `.cpp` file under `src/app/`:

    src/app/*.cpp

Read every file returned. These show how the library is used end-to-end and
reveal application-layer workflow patterns (multi-step sequences that span more
than one public API call).

### 6. Classify every capability using the decision algorithm below

For each function, method, or behaviour discovered in steps 2–5, apply this
algorithm in order. Stop at the first rule that matches.

**Actor model:**
- **User** — the application or developer calling the public API
- **System** — messageEngine (treated as a grey box)

---

**Step A — Is it called directly in `src/app/*.cpp`?**

Yes → it is user-facing. Go to Step B.
No  → it is not called directly by the user. Go to Step C.

**Step B — Does it represent a single API call or configuration action?**

Yes → classify as **HL group** (one user goal, one system response).
No, it combines two or more system calls into a multi-step pattern → classify
as **Application Workflow**.

**Step C — Is it called only from within other `src/` files (never by the user)?**

Yes → classify as **System Internal** (sub-function; invisible at the
User → System boundary).

---

**Common classification mistakes to avoid:**

- A function being `public` in C++ does NOT make it user-facing. Many internal
  helpers are `public` for testing. Apply the algorithm above, not visibility.
- `Serializer::serialize/deserialize`, `tcp_send_frame()`, `socket_send_all()`,
  `flush_delayed_to_clients()`, `recv_from_client()`, and any function not
  called in `src/app/*.cpp` are System Internals.
- `DeliveryEngine::pump_retries()` and `DeliveryEngine::sweep_ack_timeouts()`
  ARE user-facing — the application must call them in its event loop.
- A test that exercises an internal function directly (e.g. calling
  `Serializer::serialize()` in a unit test) does NOT make that function
  user-facing. Tests bypass the user boundary intentionally; classify by
  whether `src/app/` code calls it.
- Application Workflow entries are rare: only use this category when `src/app/`
  code shows a pattern of calling two or more HL-level APIs in sequence to
  accomplish one application-level goal (e.g. receive then echo back).

---

Order HL groups roughly by lifecycle:
init → send → receive → reliability helpers → impairment → simulation → observability

### 7. Write HIGH_LEVEL_USE_CASES.md

Write the complete new file using **exactly** this format:

```markdown
# messageEngine Use Case Index

Actors: **User** (application / developer) | **System** (messageEngine — grey box)

---

## HL-N: <Title>
> <One-sentence summary: user goal and system response.>

- UC_XX — <short description of this detailed flow>
- UC_YY — <short description>(parenthetical clarification if needed)

---
```

Then append the two special sections:

```markdown
## Application Workflow (above system boundary)

These use cases document patterns that combine multiple system calls and sit at
the application layer rather than at the system boundary. They are not single
User → System interactions.

- UC_XX — <description> — <one-line rationale for this classification>

---

## System Internals (sub-functions, not user-facing goals)

These use cases document mechanisms that are invoked internally by the System on
behalf of other use cases. The User never calls them directly; they are invisible
at the User → System boundary.

- UC_XX — <description> — <one-line rationale for this classification>
```

Rules:
- HL groups are numbered sequentially from HL-1.
- The `>` summary uses the established style: "User does X; System does Y."
- List entries use `UC_XX —` prefix matching any existing UC_*.md files found
  in `docs/use_cases/`; assign new UC numbers for capabilities not yet documented.
- Do not link to UC files — list entries only (links will be added when the UC
  documents are written).
- Every public API capability discovered in steps 2–3 must appear in at least
  one HL group or special section.
