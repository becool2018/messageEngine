# Standards Sources

**Project:** messageEngine
**Purpose:** Lists every external standard, guideline, or rule set referenced in `CLAUDE.md` and `.claude/CLAUDE.md`, where it comes from, and a plain-English summary of what it is.

---

## Table of Contents

1. [Coding & Safety Rules](#1-coding--safety-rules)
2. [Security Rules](#2-security-rules)
3. [NASA Software Assurance Standards](#3-nasa-software-assurance-standards)
4. [Supporting Language Standards](#4-supporting-language-standards)
5. [Contradictions and Tensions Between Standards](#5-contradictions-and-tensions-between-standards)
6. [Summary by Source](#6-summary-by-source)

---

## 1. Coding & Safety Rules

### JPL Power of 10

| Field | Value |
|---|---|
| **Full name** | JPL Power of 10 Rules for Safety-Critical Code |
| **Origin** | NASA Jet Propulsion Laboratory — Gerard J. Holzmann |
| **Published** | 2006 (IEEE Software, Vol. 23, No. 6) |
| **Availability** | Free — publicly available at NASA/JPL and IEEE |
| **Used in** | `.claude/CLAUDE.md §2`, `CLAUDE.md §9` |

**What it is:** Ten concise rules for writing safety-critical C code that can be statically analysed, reviewed, and proven correct. Originally developed at JPL for flight software and widely adopted in aerospace and safety-critical systems. The rules are intentionally restrictive — anything they prohibit is either hard to analyse or has caused real failures in deployed systems. In this project, the JPL Power of 10 rules are applied to C++ by combining the core safety constraints with a specific "F' (F-Prime) style" subset of the language. This ensures the code remains deterministic, analyzable, and suitable for safety-critical environments.

**The ten rules:**
1. Simple control flow only — no `goto`, no recursion.
2. All loops must have a fixed, statically provable upper bound.
3. No dynamic memory allocation after system initialisation.
4. Functions must be short (≤ one printed page; cyclomatic complexity ≤ 10).
5. At least two assertions per function; assertions always active in all builds.
6. Declare variables in the smallest possible scope.
7. Check every non-void return value.
8. Limit preprocessor use — no macros for control flow.
9. No more than one level of pointer indirection; no function pointers in production.
10. Zero compiler warnings; zero unresolved static analysis issues.

---

### MISRA C:2025

| Field | Value |
|---|---|
| **Full name** | MISRA C:2025 — Guidelines for the Use of the C Language in Critical Systems |
| **Origin** | MISRA (Motor Industry Software Reliability Association) |
| **Published** | 2025 (supersedes MISRA C:2012) |
| **Availability** | Proprietary — purchase required from misra.org.uk |
| **Used in** | `.claude/CLAUDE.md §3` (applies to any C files in the project) |

**What it is:** A set of required and advisory rules that restrict the C language to a safe, well-defined subset suitable for safety-critical and embedded systems. Originally developed for automotive software (hence "Motor Industry") but now used across aerospace, medical devices, and industrial control systems. Rules eliminate constructs that are undefined, implementation-defined, or difficult to statically analyse. Deviations require written justification.

---

### MISRA C++:2023

| Field | Value |
|---|---|
| **Full name** | MISRA C++:2023 — Guidelines for the Use of C++17 in Critical Systems |
| **Origin** | MISRA (Motor Industry Software Reliability Association) |
| **Published** | 2023 (supersedes MISRA C++:2008 and AUTOSAR C++14) |
| **Availability** | Proprietary — purchase required from misra.org.uk |
| **Used in** | `.claude/CLAUDE.md §3`, all C++ source files |

**What it is:** The C++ equivalent of MISRA C, targeting ISO C++17. Defines required and advisory rules that restrict C++ to a safe, analysable subset — banning features that introduce complexity, undefined behaviour, or portability hazards (exceptions, RTTI, unbounded templates, implicit conversions, etc.). The 2023 edition explicitly covers C++11/17 concurrency features including `std::atomic`, which this project uses under the standard's guidance.

---

### F´ (F-Prime) Style Subset

| Field | Value |
|---|---|
| **Full name** | F´ (F-Prime) Flight Software Framework — C++ coding style and subset |
| **Origin** | NASA Jet Propulsion Laboratory — open source |
| **Repository** | github.com/nasa/fprime |
| **Availability** | Free — Apache 2.0 licence |
| **Used in** | `.claude/CLAUDE.md §4` |

**What it is:** F´ is an open-source component-based flight software framework developed at JPL and used on multiple NASA missions (Mars Helicopter Ingenuity, ASTERIA, etc.). Its C++ coding conventions define a conservative, portable subset: no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`), no STL containers on critical paths, no templates in production code, strict `const` correctness, and a layered architecture with narrow platform adapters. This project adopts the F´ style subset as its C++ coding standard to match the portability and analysability posture of flight software.

---

## 2. Security Rules

### SEI CERT C++ Coding Standard (INT rules)

| Field | Value |
|---|---|
| **Full name** | SEI CERT C++ Coding Standard — Integer Safety Rules INT30/31/32/33-C |
| **Origin** | Carnegie Mellon University Software Engineering Institute (SEI) |
| **Availability** | Free — wiki.sei.cmu.edu/confluence/display/cplusplus |
| **Used in** | `.claude/CLAUDE.md §7a` |

**What it is:** A comprehensive set of secure coding rules for C and C++ developed by CMU's Software Engineering Institute. The integer safety subset (INT rules) addresses the class of vulnerabilities caused by integer overflow, wrap, truncation, and division by zero — common sources of exploitable bugs in network and parsing code. The four rules enforced in this project are:

- **INT30-C** — Ensure unsigned integer operations do not wrap (silent wraparound can produce incorrect sizes or loop counts that lead to buffer overflows).
- **INT31-C** — Ensure that integer conversions do not result in lost or misinterpreted data (narrowing a `uint32_t` to `uint8_t` silently drops the high bits).
- **INT32-C** — Ensure that operations on signed integers do not result in overflow (signed overflow is undefined behaviour in C++).
- **INT33-C** — Ensure that division and modulo operations do not result in divide-by-zero (undefined behaviour; can crash or be exploited as a denial-of-service vector).

---

### CWE (Common Weakness Enumeration)

| Field | Value |
|---|---|
| **Full name** | Common Weakness Enumeration |
| **Origin** | MITRE Corporation, sponsored by CISA (US Cybersecurity and Infrastructure Security Agency) |
| **Availability** | Free — cwe.mitre.org |
| **Used in** | `.claude/CLAUDE.md §7b, §7c, §7d` |

**What it is:** A community-maintained catalogue of software and hardware weakness types. Each CWE entry describes a class of defect, its consequences, and mitigations. Three CWE entries drive specific rules in this project:

- **CWE-457 — Use of Uninitialized Variable:** Reading a variable before it has been assigned a value produces undefined behaviour and unpredictable results. Mitigated by the rule that every variable must be initialised at its declaration.
- **CWE-14 — Compiler Removal of Code to Clear Buffers:** Optimising compilers are permitted to remove `memset()` calls to memory that is never subsequently read (dead-store elimination), leaving sensitive data (keys, tokens) in memory. Mitigated by using `mbedtls_platform_zeroize()` which is not subject to this optimisation.
- **CWE-208 — Observable Timing Discrepancy:** `memcmp()` returns as soon as it finds the first differing byte, leaking information about the compared value through execution time (a timing side channel). Mitigated by using `mbedtls_ct_memcmp()` which always compares all bytes in constant time.

---

### Compiler Hardening Flags

| Field | Value |
|---|---|
| **Full name** | GCC/Clang compiler and linker hardening options |
| **Origin** | GCC project, Linux kernel security team, Clang/LLVM project |
| **Availability** | Free — part of GCC and Clang toolchains |
| **Used in** | `.claude/CLAUDE.md §7e` |

**What it is:** A set of compiler and linker flags that do not change program correctness but reduce the exploitability of residual memory-safety bugs by adding runtime detection and address-space layout randomisation:

- **`-fstack-protector-strong`** — Inserts a stack canary (a secret value) before the return address in functions with local buffers. If a stack buffer overflow overwrites the canary, the program aborts before the corrupted return address is used.
- **`-D_FORTIFY_SOURCE=2`** — Enables glibc buffer-overflow detection for standard library functions (`memcpy`, `snprintf`, etc.) by substituting hardened versions that check sizes at runtime.
- **`-fPIE` / `-pie`** — Compiles and links as a position-independent executable, enabling Address Space Layout Randomisation (ASLR) which randomises the memory layout of the process on each run, making exploits harder to write.
- **`-Wl,-z,relro -Wl,-z,now`** — (Linux only) Marks the Global Offset Table and Procedure Linkage Table read-only after program startup, preventing GOT/PLT overwrite attacks.

---

## 3. NASA Software Assurance Standards

### NASA-STD-8719.13C

| Field | Value |
|---|---|
| **Full name** | NASA-STD-8719.13C — Software Safety Standard |
| **Origin** | NASA (National Aeronautics and Space Administration) |
| **Availability** | Free — standards.nasa.gov |
| **Used in** | `CLAUDE.md §13, §15, §16, §17` |

**What it is:** NASA's mandatory standard for software safety on NASA programs. It defines the process for identifying, analysing, and mitigating software hazards throughout the development lifecycle. Key obligations it imposes on this project:

- **Software Safety Hazard Analysis** — identify every way the software can cause or contribute to a hazard; record in `docs/HAZARD_ANALYSIS.md`.
- **FMEA (Failure Mode and Effects Analysis)** — for each component, enumerate failure modes and their effects; record in `docs/HAZARD_ANALYSIS.md §2`.
- **Safety-Critical (SC) function classification** — classify every public function as SC or NSC based on whether it directly affects a hazard; record in `docs/HAZARD_ANALYSIS.md §3` and annotate in source.
- **Stack depth analysis** — statically enumerate the worst-case call stack depth and size; record in `docs/STACK_ANALYSIS.md`.
- **WCET analysis** — document worst-case execution time (as operation counts) for every SC function; record in `docs/WCET_ANALYSIS.md`.

---

### NASA-STD-8739.8A

| Field | Value |
|---|---|
| **Full name** | NASA-STD-8739.8A — Assurance and Safety for Software and Automated Systems |
| **Origin** | NASA |
| **Availability** | Free — standards.nasa.gov |
| **Used in** | `CLAUDE.md §12, §13, §14` |

**What it is:** NASA's standard for software assurance practices — the "how do you know it works" complement to the "what are the hazards" standard above. It defines requirements for inspection, testing, and coverage:

- **Formal inspection process** — every non-trivial change to SC functions requires a moderator-led peer review with entry/exit criteria, defect logging, and sign-off.
- **Branch coverage floor** — 100% branch coverage of all reachable branches for every SC function.
- **MC/DC coverage goal** — Modified Condition/Decision Coverage for the five highest-hazard SC functions; requires demonstrating that each condition independently affects the outcome of each decision.

---

### NPR 7150.2D

| Field | Value |
|---|---|
| **Full name** | NPR 7150.2D — NASA Software Engineering Requirements |
| **Origin** | NASA (NASA Procedural Requirements) |
| **Availability** | Free — standards.nasa.gov |
| **Used in** | `CLAUDE.md §11, §12, §17` |

**What it is:** NASA's procedural requirements for software engineering on NASA programs. It defines what processes must be followed, what artefacts must be produced, and how software must be classified. Key obligations:

- **Bidirectional traceability** — every requirement must trace to the code that implements it and the test that verifies it; every code file must trace back to a requirement. Maintained via `// Implements: REQ-x.x` and `// Verifies: REQ-x.x` comments and `docs/TRACEABILITY_MATRIX.md`.
- **Software classification** — programs are classified Class A (mission-critical), Class B (safety-critical), or Class C (infrastructure/support). Each class has a defined minimum set of verification methods. This project is Class C with voluntary Class B rigor.
- **Independent V&V** — Class A/B require independent verification and validation; Class C does not (but the project applies Class B inspection discipline voluntarily).

---

## 4. Supporting Language Standards

### ISO C++17

| Field | Value |
|---|---|
| **Full name** | ISO/IEC 14882:2017 — Programming Language C++ |
| **Origin** | ISO (International Organization for Standardization) / IEC |
| **Published** | 2017 |
| **Availability** | Proprietary — purchase from iso.org; drafts freely available |
| **Used in** | `.claude/CLAUDE.md §4` (compile target: `-std=c++17`) |

**What it is:** The 2017 revision of the C++ language standard. Selected as the compile target because MISRA C++:2023 explicitly targets ISO C++17, and it is widely supported by GCC, Clang, MSVC, and embedded toolchains (IAR, Green Hills). C++20 and later features are explicitly excluded — they introduce constructs that MISRA C++:2023 does not yet fully cover and that embedded toolchains may not support.

---

### AUTOSAR C++14 (superseded)

| Field | Value |
|---|---|
| **Full name** | AUTOSAR C++14 Coding Guidelines |
| **Origin** | AUTOSAR (Automotive Open System Architecture) consortium |
| **Availability** | Free — autosar.org |
| **Used in** | `.claude/CLAUDE.md §3` — noted as superseded by MISRA C++:2023 |

**What it is:** A C++14-targeting coding guideline set developed by the automotive industry consortium AUTOSAR, widely used in automotive safety-critical software. MISRA C++:2023 explicitly supersedes it for C++17 projects; it is listed here only to document why the project moved on from it.

---

## 5. Contradictions and Tensions Between Standards

The standards above are largely complementary, but several genuine conflicts exist. Four are formally documented and resolved in the project's `CLAUDE.md` files; three are real but only partially addressed.

---

### Resolved conflicts

#### Conflict 1 — Power of 10 Rule 5 (always-on assertions) vs. `assert()` + `NDEBUG`
**Standards in tension:** JPL Power of 10 Rule 5 (assertions must be active in all builds) vs. the C/C++ convention that `assert()` is compiled out when `NDEBUG` is defined.
**Where documented:** `.claude/CLAUDE.md §8`, `CLAUDE.md §10`.
**Resolution:** `assert()` is prohibited in production code. All production assertions use the project macro `NEVER_COMPILED_OUT_ASSERT(cond)`, which is never disabled by `NDEBUG`. In debug builds it calls `abort()`; in production builds it calls a registered reset handler. See `src/core/Assert.hpp`.

---

#### Conflict 2 — Power of 10 universality vs. test framework needs
**Standards in tension:** JPL Power of 10 (all code must obey all rules) vs. the practical requirement that test frameworks (Catch2, etc.) internally use dynamic allocation, STL containers, function pointers, and templates — all of which Power of 10 bans.
**Where documented:** `CLAUDE.md §1b`, `CLAUDE.md §9`.
**Resolution:** An explicit per-rule exemption table in `CLAUDE.md §9` defines exactly which rules are relaxed in `tests/` only and why. Production code (`src/`) has no exemptions.

---

#### Conflict 3 — Power of 10 Rule 2 (fixed loop bounds) vs. server/networking loops
**Standards in tension:** JPL Power of 10 Rule 2 requires every loop to have a statically provable finite upper bound. TCP accept loops, UDP poll loops, and server event loops have no meaningful finite iteration count — they are designed to run for the lifetime of a connection or process.
**Where documented:** `.claude/CLAUDE.md §2.2` (infrastructure loop deviation).
**Resolution:** Designated infrastructure loops are exempt from Rule 2 provided they satisfy three conditions: (1) bounded per-iteration work, (2) a runtime termination condition (signal, timeout, connection close, or mode change), and (3) an in-code comment citing the deviation. All other loops must comply with Rule 2 as written.

---

#### Conflict 4 — Power of 10 Rule 9 (no function pointers) vs. C++ virtual dispatch
**Standards in tension:** JPL Power of 10 Rule 9 prohibits function pointers in production code. C++ virtual dispatch is implemented by the compiler using vtables, which are tables of function pointers.
**Where documented:** `.claude/CLAUDE.md §2` (Rule 9 exception), `CLAUDE.md §9`.
**Resolution:** Virtual dispatch is the approved architectural polymorphism mechanism. It is explicitly permitted as a carve-out from Rule 9 because: (1) no explicit function pointer declarations appear in application code — the compiler generates the vtable; (2) MISRA C++:2023 explicitly endorses virtual functions under its own rules; (3) it is the F´ framework's standard interface pattern. All virtual functions must conform to MISRA C++:2023 rules on virtual functions.

---

### Unresolved or partially addressed tensions

#### Tension 1 — `signal()` deviation mis-labelled
**Standards in tension:** JPL Power of 10 Rule 9 (no function pointers) and MISRA C++:2023 (restricted use of function pointers). `signal()` takes a function pointer argument with no POSIX alternative.
**Current state:** Every call site in the codebase is annotated `// MISRA deviation: signal() requires a function pointer; no alternative POSIX API available.` This correctly acknowledges the MISRA angle but does not explicitly acknowledge the Power of 10 Rule 9 deviation. The fix to the underlying tension (replacing `signal()` with `sigaction()`) does not help — `sigaction` also takes a function pointer.
**Practical impact:** Low. The deviation is documented and the use is minimal (one call per program). The labeling gap is a documentation issue, not a safety issue.

---

#### Tension 2 — CERT §7a integer safety vs. MISRA integer rules produce redundant guards
**Standards in tension:** SEI CERT INT30–33-C and MISRA C++:2023 both mandate range validation of externally-supplied integers, but from different perspectives (CERT: exploitation vectors; MISRA: implementation-defined behavior and UB). Following both rigorously produces overlapping validation guards for the same value.
**Current state:** Both are applied; the resulting code has more range checks than either standard alone would require. This mildly conflicts with JPL Power of 10 Rule 4 (short functions, CC ≤ 10) and the F´ principle of simplicity — extra guards add complexity and length.
**Practical impact:** Low in the current codebase (the overlap is small). Could become noticeable in dense parsing functions. No project-level resolution is documented; contributors are expected to apply both and accept the redundancy.

---

#### Tension 3 — Voluntary Class B practices on a formal Class C classification
**Standards in tension:** NPR 7150.2D classifies this project as Class C (infrastructure software, no direct actuator or safety-barrier control). Class C mandates only a baseline set of verification methods. The project voluntarily applies Class B obligations from NASA-STD-8739.8A: MC/DC coverage, FMEA, hazard analysis, formal state machine documentation, WCET analysis, and full inspection records.
**Current state:** The project sits between classifications — it exceeds Class C requirements but still has documented gaps before it could formally claim Class B (no TLA+/SPIN model checking, no Frama-C WP proofs, no independent V&V). See `TODO_FOR_CLASS_B_CERT.txt`.
**Practical impact:** Medium. A strict Class C audit would treat many safety artifacts as optional over-engineering. A Class B audit would find the gaps. The project's intent is to provide "flight-like" assurance without committing to a full Class B certification program; this intent is documented in `CLAUDE.md §17` but the boundary between what is required and what is voluntary is not always clear to new contributors.

---

## 6. Summary by Source

| Source | Standards |
|---|---|
| **NASA / JPL** | Power of 10, F´ style subset, NASA-STD-8719.13C, NASA-STD-8739.8A, NPR 7150.2D |
| **MISRA** | MISRA C:2025, MISRA C++:2023 |
| **SEI / CMU** | CERT C++ INT30-C, INT31-C, INT32-C, INT33-C |
| **MITRE / CISA** | CWE-457, CWE-14, CWE-208 |
| **ISO / IEC** | ISO C++17 |
| **GCC / Clang / Linux kernel** | Compiler hardening flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, `-fPIE`, RELRO) |
| **AUTOSAR** | C++14 guidelines (superseded; noted for historical context) |

---

*This document is manually maintained. When a new standard is added to `CLAUDE.md` or `.claude/CLAUDE.md`, add a corresponding entry here.*
