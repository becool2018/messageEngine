# Verification and Validation Policy

**Document ID:** VVP-001
**Authority:** NPR 7150.2D, NASA-STD-8739.8A, NASA-STD-8719.13C
**Applicability:** All software products developed under this policy

---

## 1. Purpose

This document defines the minimum verification evidence required to
assert correctness of software at each classification level. It is
normative. A verification method not listed as sufficient for a given
classification is explicitly insufficient, regardless of the quality
of its execution.

Although the software classification is Class C, all verification
activity voluntarily meets the method requirements of Class B (M1 + M2
+ M4 + M5 for all functions). This is a voluntary elevation of testing
rigor, not a reclassification.

---

## 2. Definitions

**2.1 Safety-Critical (SC) function**
A function whose incorrect behavior directly contributes to a hazard
identified in the project Software Safety Hazard Analysis. SC functions
are identified per project in the Hazard Analysis (§3 of that document).

**2.2 Non-Safety-Critical (NSC) function**
A function with no direct effect on safety-critical behavior.
Observability, configuration, initialization, and lifecycle functions
are typically NSC unless analysis shows otherwise.

**2.3 Reachable branch**
A branch outcome (True or False of a decision) for which there exists
at least one input or system state that causes execution to follow
that path. A branch is unreachable only when a written architectural
argument demonstrates no such input or state can exist.

**2.4 Fault injection**
A test technique in which the behavior of an underlying dependency
(operating system call, hardware driver, communication library) is
programmatically controlled to return error values that cannot be
induced in a nominal test environment. Fault injection requires an
injectable interface — a seam in the design that allows the real
dependency to be replaced with a test-controlled substitute.

**2.5 Architectural ceiling**
A documented, reviewed justification that one or more branch outcomes
are permanently unreachable due to an architectural property of the
implementation (e.g., a `[[noreturn]]` assertion path, a platform
endianness constant, a mathematically-provable invariant).
Architectural ceiling justifications are subject to the constraints
in §4.3.

---

## 3. Verification Methods

| ID | Name | Description | Evidence |
|----|------|-------------|----------|
| M1 | Code review / inspection | Structured examination of source code by a reviewer independent of the author. | Signed inspection record per project inspection process. |
| M2 | Static analysis | Automated tool analysis of source code without execution. Includes compiler warnings (zero-warnings policy), linting, and MISRA compliance checking. | Tool output with zero unresolved findings on the exact source revision under review. |
| M3 | Dynamic test, line coverage | Test execution in which every source line of the function under test is reached at least once. | Instrumented coverage report showing 100% line coverage, or documented justification for any uncovered line. |
| M4 | Dynamic test, branch coverage | Test execution in which every reachable branch outcome of every decision in the function under test is reached at least once. | Instrumented branch coverage report showing 100% coverage of reachable branches. Any branch claimed unreachable must be supported by a written architectural ceiling argument (§4.3). |
| M5 | Dynamic test, fault injection | Test execution that exercises error-handling paths unreachable in a nominal environment by substituting real dependencies with test-controlled implementations. | Branch coverage of error-handling paths confirmed by instrumented coverage report; injectable interface and substitute implementation under version control and subject to the same review requirements as production code. |
| M6 | MC/DC coverage | Test execution demonstrating that every condition in every decision independently affects the decision outcome. | Test-case review matrix demonstrating independence for each condition. |
| M7 | Formal verification | Model checking or theorem proving of correctness properties. | Model or proof artifact under version control with no unresolved counterexamples or proof obligations. |

---

## 4. Minimum Verification Requirements by Classification

### 4.1 Required methods

| Classification | SC functions | NSC functions |
|----------------|-------------|---------------|
| Class C | M1 + M2 + M4 | M1 + M2 + M3 |
| Class B | M1 + M2 + M4 + M5 | M1 + M2 + M4 |
| Class A | M1 + M2 + M4 + M5 + M6 + M7 (for SC state machines) | M1 + M2 + M4 + M5 |

Methods are cumulative. Class B requires M4 **and** M5, not M4 **or** M5.

### 4.2 Constraint on loopback-only test environments

A test environment that exercises only nominal operating conditions
(e.g., loopback network, no resource exhaustion, no injected faults)
satisfies M4 only for branches reachable under those conditions.

Branches reachable exclusively via dependency failure (operating
system errors, library errors, hardware faults) require M5 to satisfy
M4. Documenting such branches as an architectural ceiling is valid
only at Class C. It is **not** valid at Class B or Class A.

**Consequence:** any module containing error-handling paths that depend
on an underlying dependency that cannot fail in the nominal test
environment MUST provide an injectable interface before it can be
verified to Class B or Class A standard.

### 4.3 Architectural ceiling constraints

An architectural ceiling argument is valid only when **all** of the
following are true:

a) The unreachable branch is identified precisely by file, line,
   and branch direction (True or False).

b) A written argument is provided explaining why no input or system
   state can cause the branch to be taken.

c) The argument is reviewed and signed off by a reviewer independent
   of the author.

d) The argument is limited to one of the following categories:

   - **i. `[[noreturn]]` assertion abort paths** — the True (fires)
     branch of an assertion macro backed by `abort()` or equivalent;
     LLVM source-based coverage cannot instrument this path.

   - **ii. Platform architecture constants** — branch outcomes
     determined entirely at compile time by target architecture
     (e.g., byte-order conditionals in system macros on a
     known-endian platform).

   - **iii. Mathematically-provable invariants** — branch outcomes
     excluded by a class invariant or loop invariant that can be
     stated as a formal postcondition and verified by inspection
     or proof.

e) The ceiling argument does **not** include:

   - **i. Dependency failure paths** (operating system, library,
     hardware) that are unreachable only because the test environment
     does not induce failures. These require M5, not a ceiling.

   - **ii. Paths unreachable only because no test was written.**
     These are test gaps, not architectural ceilings.

At Class B and Class A, categories d-i and d-ii remain valid.
Category d-iii requires written proof, not inspection alone.
Category e-i is never valid at any classification level as a
permanent ceiling; it is a temporary gap requiring M5.

**Relationship to per-file thresholds in `docs/COVERAGE_CEILINGS.md`:**
The policy floor is **100% of reachable branches** — this is not negotiable
regardless of classification level. The numeric thresholds listed in
`docs/COVERAGE_CEILINGS.md` (e.g., ≥70%, ≥74%) are *regression guards* set
at the current maximum achievable given category d-i (`NEVER_COMPILED_OUT_ASSERT`
`[[noreturn]]` True paths) and, in a few files, category d-iii
(mathematically-provable dead branches). They are not policy floors, not
acceptable coverage targets, and not evidence of Class B compliance gaps.
Power of 10 Rule 5 (≥2 assertions per function) is the structural cause: every
function contributes at least 2 permanently-missed LLVM branch outcomes. A
threshold that falls below its documented value is a defect; a threshold that
rises is an improvement that must be reflected in the table.

### 4.4 Injectable interface requirement

Any module whose correctness verification at Class B or Class A
requires M5 MUST be designed with an injectable interface that
allows every underlying dependency to be replaced with a
test-controlled substitute. This is an **architectural requirement**
on the module design, not merely a testing requirement.

An injectable interface must:

a) Cover every external dependency call whose failure constitutes
   an error-handling path in the module.

b) Be itself subject to M1 + M2 + M3 verification.

c) Have at least one production implementation and at least one
   test substitute implementation, both under version control.

d) Not introduce dynamic allocation, function pointers visible at
   the call site, or other constructs prohibited by the applicable
   coding standard.

---

## 5. Verification Evidence Traceability

Every test file must carry a comment identifying either the
verification methods applied OR the requirements verified:

```cpp
// Verification: M1 + M2 + M4
// Verification: M1 + M2 + M4 + M5 (fault injection via <InterfaceName>)
// — OR —
// Verifies: REQ-x.x[, REQ-y.y ...]   (per CLAUDE.md §11 traceability policy)
```

Both forms satisfy the evidence traceability requirement. The
`// Verifies: REQ-x.x` form is the project standard per CLAUDE.md §11;
the `// Verification: M1 + M2 + M4` form may be added for additional
clarity at Class B or Class A.

Every SC function declaration must carry a comment identifying the
applicable hazards, and optionally the highest verification method
achieved:

```cpp
// Safety-critical (SC): HAZ-NNN[, HAZ-NNN...]
// Safety-critical (SC): HAZ-NNN — verified to M4   (optional M-label)
// Safety-critical (SC): HAZ-NNN — verified to M5   (optional M-label)
```

A mismatch between the declared method and the evidence in the
coverage report or inspection record is a **MAJOR** defect per the
project inspection checklist.

---

## 6. Classification Change Process

Reclassifying a software product or module from Class C to Class B
or Class A requires, before any new verification activity begins:

a) Identification of all modules containing error-handling paths
   that depend on dependencies that cannot fail in the nominal
   test environment.

b) Design and implementation of injectable interfaces for all such
   modules, reviewed to M1 + M2 + M3.

c) Implementation of fault-injection test suites achieving M4 on
   the previously-uncovered error-handling paths.

d) Update of all SC function declarations and test file headers to
   reflect the achieved verification method.

e) Update of the Hazard Analysis and FMEA to reflect any new hazards
   identified during the injectable interface design.

Reclassification is not complete until all items above are done and
signed off in the project inspection record.

### 6.1 Current completion status (as of 2026-04-09)

| Item | Requirement | Status |
|------|-------------|--------|
| a | Identify modules with untestable error-handling paths | **COMPLETE** — All four transport backends (TcpBackend, UdpBackend, TlsTcpBackend, DtlsUdpBackend) identified. POSIX socket/TLS/DTLS dependency calls cannot fail in a loopback test environment. |
| b | Design and implement injectable interfaces | **COMPLETE** — `ISocketOps` (src/platform/ISocketOps.hpp) with `MockSocketOps` test double covers TcpBackend and UdpBackend. `IMbedtlsOps` (src/platform/IMbedtlsOps.hpp) with `DtlsMockOps` covers TlsTcpBackend and DtlsUdpBackend. Both interfaces reviewed to M1 + M2 + M3. |
| c | Implement fault-injection test suites (M5) | **COMPLETE** — 15 M5 tests added in INSP-019 (commits 681cedd, 0b6918e) exercise all POSIX error-handling paths across all four backends. SC function `.hpp` declarations carry `— verified to M5` annotations. Test file headers carry `// Verification: M1 + M2 + M4 + M5 (fault injection via ISocketOps)` or equivalent. |
| d | Update SC declarations and test headers | **COMPLETE** — All SC function declarations in TcpBackend.hpp, UdpBackend.hpp, TlsTcpBackend.hpp, DtlsUdpBackend.hpp carry `— verified to M5`. All four backend test files carry `Verification: M1 + M2 + M4 + M5` headers. |
| e | Update Hazard Analysis and FMEA | **COMPLETE** — HAZARD_ANALYSIS.md §2 and §3 updated through INSP-018 to reflect the injectable interface design; no new hazards were identified during the M5 test suite implementation. |

> **Note:** The "verified to M5" annotation in item (d) applies to the SC functions covered by
> INSP-019 (scope: send/receive paths as of 2026-04-09). SC functions added in subsequent
> security hardening PRs — including `TcpBackend::sweep_hello_timeouts`, `UdpBackend::init`,
> `TlsTcpBackend::recv_from_client`, `DtlsUdpBackend::init`,
> `DtlsUdpBackend::client_connect_and_handshake`, `DtlsUdpBackend::send_hello_datagram`,
> `IMbedtlsOps::recvfrom_peek`, and related `init()` functions — do not yet carry the
> "verified to M5" annotation. These functions require their own M5 verification evidence
> entries, to be added when those functions are formally inspected.

**Remaining gap before formal Class B reclassification:** Items a–e are complete. The remaining
obligation (per NPR 7150.2D §3.11) is independent V&V of all SC functions — a process gate
requiring a human reviewer who was not involved in the implementation. See
`TODO_FOR_CLASS_B_CERT.txt` for the full gap list including TLA+/SPIN model checking and
Frama-C WP theorem proving requirements.
