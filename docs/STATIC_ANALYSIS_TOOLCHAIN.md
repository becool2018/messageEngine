# Static Analysis Toolchain — messageEngine

Policy: CLAUDE.md §9.1 / NPR 7150.2D / NASA-STD-8739.8

NASA projects must identify static analysis tools explicitly because different
tools have different rule sets, MISRA coverage levels, and false-positive rates
(NPR 7150.2D §3). This file is the authoritative toolchain definition.

Tools are organised into four tiers by when they run and what they enforce.

---

## Tier 1 — Build-time (every compile) — ACTIVE

**Tool:** GCC / Clang compiler warnings

Flags (compiler-family conditional — see note below):
```
-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
-Wcast-align -Wformat=2 -Wnull-dereference -Wdouble-promotion
-Wno-unknown-pragmas
-fstack-protector-strong -fPIE

# Clang builds only:
-Wpedantic -Wno-gnu-zero-variadic-macro-arguments

# GCC builds only (no -Wpedantic — see note):
-Wno-variadic-macros
```

Note — compiler-family detection: The Makefile detects whether `$(CXX)` is
Clang or GCC at configuration time (via `$(CXX) --version`). On Clang,
`-Wpedantic` is retained and only the GNU-extension warning for zero-argument
`##__VA_ARGS__` is suppressed with `-Wno-gnu-zero-variadic-macro-arguments`.
On GCC, `-Wpedantic` is omitted entirely because GCC has no subcategory flag
for the zero-argument `##__VA_ARGS__` warning; `-Wno-variadic-macros` is
applied instead.

`-fstack-protector-strong` and `-fPIE` are compile-time flags (present in
`CXXFLAGS`) applied to every object file, not only at link time.

Role: First line of defence. Zero-warnings policy (Power of 10 Rule 10) means
any warning is a build failure. Enforced in `Makefile`.

Status: **ACTIVE**

---

## Tier 2 — Fast CI pass (every change, before merge)

### Clang-Tidy — ACTIVE

Makefile target: `make lint`

Role: Day-to-day enforcement of Power of 10 patterns, MISRA-adjacent checks,
complexity limits, and the per-directory compliance table (CLAUDE.md §9).

Config:
- `src/.clang-tidy` — strict profile (full rule set)
- `tests/.clang-tidy` — relaxed profile (STL and dynamic alloc permitted)

Key checks enabled (representative; see `src/.clang-tidy` for the full list):
- `readability-function-cognitive-complexity` (threshold: 10, per Rule 4)
- `cppcoreguidelines-no-malloc` (Rule 3)
- `cppcoreguidelines-pro-type-cstyle-cast` (no C-style casts)
- `misc-no-recursion` (Rule 1)
- `readability-const-return-type`, `readability-non-const-parameter` (const correctness)
- `bugprone-*`, `clang-analyzer-*`, `performance-*`, `portability-*`
- `fuchsia-restrict-system-includes` — not currently enabled in src/.clang-tidy

Status: **ACTIVE**

### Cppcheck — ACTIVE

Makefile target: `make cppcheck`

Role: Second-pass analysis; catches patterns Clang-Tidy misses — integer
overflow, uninitialised variables, out-of-bounds array access, and style
issues. Suppressions for documented false positives are in `.cppcheck-suppress`.

Config: `make cppcheck` (CI-safe target) does **not** pass `--addon=misra`; it
runs Cppcheck without the MISRA addon for fast CI feedback. The separate
`make cppcheck-misra` target passes `--addon=misra` on `src/` (see below).

Status: **ACTIVE**

### Clang Static Analyzer — ACTIVE

Makefile target: `make scan_build` (also included in `make static_analysis` umbrella)

Role: Path-sensitive interprocedural analysis for null dereference, dead store,
uninitialized values, and memory leaks. Complementary to Clang-Tidy (which is
primarily pattern-based) because the Static Analyzer tracks values across calls.

Config: `scan-build` wrapper over the standard build; reports written to
`build/scan-build/`.

Checks: All default Clang Static Analyzer checkers (null dereference, dead store,
uninitialized values, memory leaks) plus three alpha checkers explicitly enabled:
- `-enable-checker alpha.core.BoolAssignment`
- `-enable-checker alpha.core.Conversion`
- `-enable-checker alpha.unix.cstring.OutOfBounds`

Status: **ACTIVE**

---

## Tier 3 — MISRA compliance pass (before any release or external review)

### PC-lint Plus (primary) — PENDING

Makefile target: `make pclint`

Role: Authoritative MISRA C++:2023 compliance checker. Produces the compliance
report required for NPR 7150.2D formal assurance reviews. PC-lint Plus is the
most widely accepted MISRA C++ checker in NASA and DO-178C contexts. Covers all
Required and Advisory rules; generates per-rule deviation reports.

Config (to be created):
- `pclint/co-gcc.lnt`
- `pclint/misra_cpp_2023.lnt`
- Separate `.lnt` option files for `src/` (strict) and `tests/` (relaxed)

Status: **TODO — requires licence purchase**

### Cppcheck with MISRA addon (alternative) — ACTIVE (macOS only)

Makefile target: `make cppcheck-misra`

Role: Covers a subset of MISRA C++:2023 rules. Acceptable for development-time
checking but does not produce a full compliance report suitable for formal audit.
Use as a stand-in until PC-lint Plus is procured.

Invocation: `--addon=misra --std=c++17 -I src` (no version qualifier; misra.py
selects the applicable rule set from its bundled data).

Platform restriction: `make cppcheck-misra` is macOS-only (local use). It is
**excluded from CI** because cppcheck 2.13 (Ubuntu 24.04 apt package) fails when
`--addon=misra` is combined with `--suppressions-list`; the CI target uses the
plain `make cppcheck` instead. Run `make cppcheck-misra` locally on macOS
(`brew install cppcheck` includes the misra.py addon).

Status: **ACTIVE** (macOS local use only; CI uses `make cppcheck` without addon)

---

## Tier 4 — Deep formal analysis (pre-release or on CRITICAL changes) — NOT REQUIRED

### Polyspace Bug Finder / Code Prover (MathWorks)

Role: Formal proof of absence of runtime errors (null deref, overflow, data
races). Required for Class A software under NPR 7150.2D. Optional at current
Class C classification.

Status: **NOT REQUIRED** — revisit if classification is raised to Class A/B.

### Coverity Static Analysis (Synopsys)

Role: Deep interprocedural analysis; excellent at finding concurrency defects and
subtle memory errors. Widely used by NASA and DoD projects.

Status: **NOT REQUIRED** — revisit if classification is raised.

---

## TODO list

- [x] Install Clang-Tidy; create `src/.clang-tidy` (strict) and `tests/.clang-tidy` (relaxed).
- [x] Add `make lint` target invoking Clang-Tidy on `src/` and `tests/`.
- [x] Install Cppcheck; create `.cppcheck-suppress` deviation file.
- [x] Add `make cppcheck` target invoking Cppcheck (no MISRA addon) on `src/` — CI-safe.
- [x] Add `make cppcheck-misra` target invoking Cppcheck + MISRA addon on `src/` — macOS local use only.
- [ ] Procure PC-lint Plus licence; create `pclint/` config directory.
- [ ] Add `make pclint` target for formal MISRA C++:2023 compliance report.
- [x] Add `make static_analysis` umbrella target running `lint` + `cppcheck` + `scan_build` in sequence.
- [ ] Document all tool-reported deviations in `docs/DEFECT_LOG.md` with WAIVE
      disposition and the specific MISRA rule reference.
- [ ] Add Clang-Tidy or cppcheck rule to flag any remaining `assert()` in `src/`
      (CLAUDE.md §10 — `NEVER_COMPILED_OUT_ASSERT` required in all production code).
- [ ] Investigate enabling `fuchsia-restrict-system-includes` in `src/.clang-tidy`
      once system-header include paths are stabilised.
- [ ] Generate a `compile_commands.json` (via CMake or Bear) to eliminate
      `misra-config` false-positive warnings in `make cppcheck-misra`.
