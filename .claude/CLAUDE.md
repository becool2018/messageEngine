# CLAUDE.md

## 1. How to use this file

- These instructions apply to all C/C++ code in this repository.
- Always follow the **Global C/C++ Coding Standard** first.
- Then apply any **Architecture & Layering** and **Module-Specific Requirements** relevant to the files you are touching.
- If a user request conflicts with these rules, follow these standards and briefly explain the conflict in your response.

---

## 2. Global C/C++ Coding Standard

```xml
<global_c_cplusplus_standard>
This project uses C and C++ only. All code must comply with the following global standards, regardless of module or file.

1. Languages and overall safety posture
- Implementation languages: C and/or C++ only.
- All code (production, tests, and tools where feasible) must:
  - Obey the JPL “Power of 10” rules for safety-critical C.
  - Obey MISRA C:2025 (for any C code) and MISRA C++:2023 (for all C++ code), with documented deviations only when absolutely necessary.
  - Obey a flight-like F´ (F-Prime) style subset for all C++ code.
- Safety levels:
  - Production code (src/) is treated as high criticality; all rules apply without
    exception unless an explicit deviation is documented.
  - Test code (tests/) must obey all non-negotiable rules listed below but may relax
    a defined subset of rules to accommodate test-framework idioms — see §3 for the
    exact permitted relaxations. No rule relaxation is permitted in src/.

2. Power of 10 rules (enforced on all code)
Apply these rules in every file:
1) Simple control flow only
   - No goto, no setjmp/longjmp.
   - No direct or indirect recursion.
2) Fixed loop bounds
   - Every loop must have a statically provable upper bound that a static analysis tool can verify.
   - Exception - Certain infrastructure loops (TCP accept loop, poll/recv loop, application event loop) are designed to operate for 
     the lifetime of the process or connection and therefore have no meaningful finite iteration bound. These loops are treated as 
     non‑terminating scheduler‑style iterations as allowed by the Power of 10 rationale.
     Deviation: For these designated loops, the project does not require a statically provable finite iteration bound. 
     Instead, they must: 
       (1) have statically bounded per‑iteration work, 
       (2) be governed by runtime termination conditions (signal, timeout, connection close, or mode change), and 
       (3) include an in‑code comment referencing this deviation. All other loops must comply with Rule 2 as written.”
3) No dynamic allocation after initialization
   - No malloc/new or equivalent on safety-critical paths after system initialization.
   - Prefer static or stack allocation; any required heap allocation must occur during an explicit init phase.
4) Short, single-purpose functions
   - Functions must remain small and cohesive, no longer than one printed page in a standard coding style.
   - Cyclomatic complexity must not exceed 10 per function (NASA standard numeric ceiling).
5) Assertion density
   - At least two assertions per function — this is an individual per-function minimum,
     not a fleet average. A function with zero assertions is non-compliant regardless of
     how many assertions appear in other functions.
   - Use assertions to enforce critical invariants, preconditions, and postconditions.
6) Minimal variable scope
   - Declare variables in the smallest possible scope consistent with clarity.
7) Check all return values
   - Every non-void function return value must be checked explicitly by the caller.
   - Ignoring return values is not allowed.
8) Limit preprocessor usage
   - Do not use macros for control flow or to obscure structure.
   - Prefer const, enums, and inline functions instead of complex macros.
9) Restrict pointer usage
   - No more than one level of pointer indirection per expression.
   - Function pointers are prohibited in production code.
   - Exception — compiler-generated vtables backing C++ virtual functions are permitted.
     Rationale: virtual dispatch is the architectural mechanism for TransportInterface
     polymorphism; it is endorsed by MISRA C++:2023 rules on virtuals; and it produces
     no explicit function pointer declarations in application code.
     All virtual functions must conform to MISRA C++:2023 rules on virtual functions.
10) Zero-warnings policy
   - Compile with the strictest warning settings available (e.g., -Wall -Wextra -Wpedantic or equivalent).
   - All code must compile with zero warnings.
   - Code must pass static analysis with zero unresolved issues; violations must be fixed or explicitly justified and minimized.

3. MISRA C:2025 / MISRA C++:2023 compliance
- Versions in use:
  - C code:   MISRA C:2025.
  - C++ code: MISRA C++:2023 (targets ISO C++17; supersedes MISRA C++:2008 and AUTOSAR C++14).
- Rule categories (MISRA C++:2023 uses two tiers):
  - Required rules must be met; deviations require written justification.
  - Advisory rules are followed by default; conscious deviations are documented.
- Any deviation must be minimal, justified, and clearly documented in a comment at the point of use.
- Where Power of 10 or the F´ subset is stricter than MISRA, follow the stricter rule.
- Assume static analysis tools are configured for MISRA C++:2023; write code so they can check compliance easily.
- Concurrency and atomics under MISRA C++:2023:
  - MISRA C++:2023 explicitly covers C++11/17 concurrency features (threads, mutexes, std::atomic).
  - std::atomic is the approved, standards-compliant approach for lock-free shared state.
  - std::atomic is explicitly permitted in this project as a carve-out from the general no-STL
    rule (see section 4). It has no dynamic allocation, maps directly to hardware primitives, and
    is portable across all conforming toolchains — which aligns with the F´ portability principle.
  - GCC __atomic built-ins must not be used; prefer std::atomic for all atomic operations.

4. F´-style C++ subset (all C++ code)
- Language standard:
  - Target ISO C++17 to align with MISRA C++:2023 (compile with -std=c++17).
  - Do not use C++20 or later features.
- Exceptions:
  - Exceptions are disabled (compile with -fno-exceptions).
  - No throwing or catching exceptions.
- Templates and STL:
  - No C++ templates in production code, except as explicitly listed below.
  - Do not use the C++ Standard Template Library (STL) in production code, except as explicitly listed below.
  - This explicitly includes <thread>, <mutex>, <memory>, and all container/algorithm headers.
  - Rationale: STL containers and algorithms pull in significant implementation complexity that
    cannot be audited or statically analysed to MISRA/Power-of-10 depth on embedded targets.
  - Exception — std::atomic<T> for integral types is permitted:
    - It has no dynamic allocation and maps directly to a hardware primitive.
    - It is ISO standard (C++11/17), portable across GCC, Clang, MSVC, and IAR.
    - It is what MISRA C++:2023 endorses for lock-free shared state.
    - Portability across toolchains is a core F´ principle; std::atomic satisfies it;
      compiler-specific __atomic built-ins do not.
    - Include <atomic> only in files that actually need atomic operations.
- RTTI:
  - No dynamic_cast, typeid, or other runtime type information.
  - Compile with -fno-rtti.
- Casting:
  - Prefer static_cast for all routine conversions; it is the default safe cast.
  - reinterpret_cast is permitted only where MISRA C++:2023 Rule 5.2.4 explicitly
    allows it (e.g., converting between pointer-to-object and pointer-to-byte for
    hardware-register or serialization access). Every use must be accompanied by a
    comment citing the specific MISRA 5.2.4 permission and the reason no safer cast
    suffices. Treat it as a deviation, not a routine tool.
  - const_cast is permitted only to remove const imposed by a third-party or OS API
    signature that is known not to modify the object; document each use.
  - Never use C-style casts; they silently select the most dangerous applicable cast.
  - Avoid dynamic_cast entirely (RTTI is disabled).
- Error handling philosophy:
  - Prefer continued operation with localized recovery where possible.
  - Use an event/logging model conceptually similar to F´:
    - WARNING_LO: localized, recoverable issues.
    - WARNING_HI: system-wide but recoverable issues.
    - FATAL: unrecoverable failures requiring restart/reset of the affected component.

5. Architecture, layering, and portability
- Clear layering:
  - Keep OS/platform-specific code in narrow adapter layers.
  - Higher-level logic depends on lower-level abstractions, not directly on OS APIs.
- Dependency direction:
  - Lower layers must not depend on higher layers; avoid cyclic dependencies.
- Portability:
  - Assume POSIX-like platforms by default.
  - Isolate non-portable code to well-defined boundaries so it can be mocked or replaced.

6. Non-functional global requirements
- Real-time / performance:
  - No hard real-time guarantees unless explicitly stated in module requirements.
  - Avoid unnecessary blocking and unbounded work in critical paths; design for predictable timing and bounded resource usage.
- Memory and resources:
  - Avoid unbounded growth of memory, file descriptors, sockets, threads, etc.
  - Use explicit limits and fail-safe behavior when limits are reached.
- Testability:
  - Code should be unit-testable with external dependencies replaceable by mocks or simulation harnesses.
  - Avoid hidden global state that makes testing difficult.

7. Security and robustness posture
- Input validation:
  - Validate all external inputs (network, files, hardware, user data).
  - Never trust sizes, formats, or ranges without checks.
- Undefined behavior:
  - Avoid constructs that can cause undefined or implementation-defined behavior.
- Secure by default:
  - Prefer safe defaults (e.g., encryption enabled, conservative logging) unless explicitly relaxed for tests.
- Logging and privacy:
  - Log enough for debugging and safety analysis.
  - Avoid logging sensitive payload contents by default; prefer metadata and identifiers.

8. NASA-style software engineering and assurance mindset
- Code must be analyzable, reviewable, and testable.
- Assume formal inspections and structured reviews for important artifacts.
- Maintain traceability:
  - Requirements → tests → code, and code → requirements.
- Readability:
  - Code should read as if one careful engineer wrote it in one sitting.
  - Keep naming, formatting, and structure consistent.
- Assertions and defensive programming:
  - Use assertions and defensive checks to detect contract violations early.
  - Never use raw assert() in production code. Use NEVER_COMPILED_OUT_ASSERT(cond) instead.
    NEVER_COMPILED_OUT_ASSERT is never disabled by NDEBUG — it is always compiled in and
    always evaluated, in every build. Behavior by build type:
      Debug / test: logs condition + file + line, then calls abort() for immediate crash
                    and stack trace.
      Production:   logs FATAL with condition + file + line, then triggers a controlled
                    component reset. Does not call abort() or terminate the process.
    Rationale: JPL Power of 10 Rule 5 requires assertions to remain active in all builds.
    The prior wording ("fail fast in debug builds while preserving safe behavior in
    production") implied NDEBUG-disabled assertions in production, which directly conflicts
    with Rule 5. NEVER_COMPILED_OUT_ASSERT resolves this — see CLAUDE.md §10.
- Static analysis:
  - Code is written assuming regular static analysis and MISRA checks.
  - Avoid patterns that defeat or confuse analyzers.

9. Configuration management, observability, and traceability
- Version control:
  - All code, tests, and configuration live under version control.
  - Each change should be logically small and explainable.
- Traceability:
  - Each module/function should map to one or more requirements.
  - Avoid dead or untraceable functionality.
- Observability:
  - Provide logging and metrics hooks so higher-level systems can observe latency, error rates, and restart/fatal events over time.

10. Practical instructions for Claude
When editing or generating C/C++ code, I must:
- Respect all Power of 10 rules, MISRA rules, and the F´-style C++ subset.
- Avoid recursion, runtime allocation on critical paths, deep pointer chains, macros that affect control flow, exceptions, templates, STL, RTTI, and explicit function pointer declarations. Vtable-backed virtual dispatch is permitted per the Rule 9 exception above.
- Keep functions small, cohesive, and well-asserted.
- Use only language constructs acceptable under both MISRA and Power of 10.
- Structure code to compile cleanly with strict warnings and pass MISRA-oriented static analysis.
- Preserve clear layering, testability, and portability in designs and APIs.
- Explicitly comment any unavoidable deviation and keep it as small as possible.
- Traceability: follow the rules in ./CLAUDE.md §11 (traceability policy)
  and ./CLAUDE.md §13 (safety requirements). Run docs/check_traceability.sh
  after any change.
</global_c_cplusplus_standard>
```

---

## 3. Architecture and Layering

```xml
<architecture_and_layers>
- src/core:
  - Core domain logic.
  - Depends only on abstract interfaces declared in src/platform (e.g.,
    TransportInterface); no direct POSIX, socket, or thread API calls.
  - Must not include or call concrete platform implementations directly.
- src/platform:
  - OS and platform adapters (sockets, timers, threading, filesystem).
  - No application-specific policy; only mechanics and thin wrappers.
- src/app:
  - Application orchestration and high-level behavior.
  - Uses src/core and src/platform through their public interfaces.
- tests:
  - Unit and integration tests.
  - Must obey all non-negotiable rules: no undefined behaviour, no C-style casts,
    no goto/recursion, no exceptions (compile flag enforced), no C++20+ features,
    zero compiler warnings, and all return values checked.
  - May relax the following rules to accommodate test-framework idioms:
    - STL containers (e.g., std::vector, std::string) are permitted for test
      fixture setup and data construction only; not on paths under test.
    - Templates are permitted for test helper utilities (e.g., typed test tables).
    - Dynamic allocation (new/delete) is permitted in test fixture setup and
      teardown; not on the path that exercises production code.
    - The per-function assertion minimum (Rule 5) is relaxed to one assertion per
      test function when the function body is a single VERIFY/CHECK call.
    - Cyclomatic complexity ceiling (Rule 4, ≤10) is raised to ≤15 for test
      functions that necessarily exercise many code paths in sequence.
    - MISRA advisory rules may be relaxed where the test framework requires it;
      MISRA required rules remain in force.
  - No other relaxations are permitted. When in doubt, follow the production rules.
- docs:
  - Requirements, design docs, and other project documentation.

Layering rules:
- Higher layers may depend on lower ones (app → core → platform), never the reverse.
- No bypassing abstractions:
  - src/app and src/core must not call raw OS APIs or sockets directly; only through src/platform interfaces.
- No cyclic dependencies between modules or directories.
</architecture_and_layers>
```

---

## 4. Module-Specific Requirements (placeholder)

You can expand these as your design solidifies.

```xml
<module_requirements name="network_layer">
- Implements shared message envelopes, delivery semantics, and any impairment/simulation logic.
- Depends only on abstract transport interfaces (e.g., from src/platform), not raw sockets.
- Respects all global standards: no dynamic allocation after init on critical paths, Power of 10, MISRA, F´ subset.

<module_requirements name="platform_network">
- Encapsulates TCP/UDP sockets and optional TLS/DTLS.
- Handles framing, timeouts, basic error mapping, and connection management.
- No application-level policy decisions (retries, routing, business logic); only transport mechanics.
</module_requirements>
```

Add more `<module_requirements>` blocks as you define components.

---

## 5. Testing, Analysis, and CI Expectations

```xml
<testing_and_analysis>
- All non-trivial code should have unit tests where feasible.
- Design code to be testable without real network/hardware (via mocks, simulation harnesses, or dependency injection).
- Assume:
  - Strict compiler warnings are enabled in CI.
  - Static analysis and MISRA checking run regularly.
- Prefer small, focused changes that are easy to reason about and review.
</testing_and_analysis>
```

---

## 6. Instructions for Claude

```xml
<instructions_for_claude>
1. Before making significant changes, briefly summarize how the planned change fits:
   - <global_c_cplusplus_standard>
   - <architecture_and_layers>
   - Any relevant <module_requirements>.
2. If a user request conflicts with the global standard, explain the conflict and propose a compliant alternative instead of blindly following the request.
3. When you apply a specific rule that strongly shapes the design, add a short comment, e.g.:
   - "// Power of 10: no dynamic allocation after init — using static buffer"
   - "// Power of 10: bounded loop with explicit max_iterations"
   - "// Power of 10 Rule 2 deviation: infrastructure event loop — bounded per-iteration
   //   work, terminates on signal/timeout/close per CLAUDE.md §2.2 exception."
4. Favor simpler, safer designs over clever or complex ones, especially where they improve analyzability and testability.
</instructions_for_claude>
```

---

## 7. Open Questions / TODOs

```xml
<open_questions>
- MISRA versions are resolved: MISRA C:2025 for C code, MISRA C++:2023 for C++ code.
- Compiler standard is resolved: -std=c++17 in use.
- std::atomic carve-out is resolved: std::atomic<T> for integral types is permitted;
  GCC __atomic built-ins are no longer used anywhere in the codebase.
- Performance targets are addressed in §14 (WCET) and docs/WCET_ANALYSIS.md;
  per-module resource limits are the compile-time constants in src/core/Types.hpp.
- Test-only relaxations are resolved: see §3 (Architecture and Layering, tests entry)
  for the exact enumerated list of permitted relaxations and non-negotiable rules.
</open_questions>
```
