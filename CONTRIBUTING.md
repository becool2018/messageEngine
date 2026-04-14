# Contributing to messageEngine

Thank you for your interest in contributing. messageEngine is a safety-critical
networking library built to NASA/JPL Power of 10, MISRA C++:2023, and NPR 7150.2D
standards. All contributions must meet the same bar as the existing codebase.

---

## Before you start

1. **Open an issue first** for any non-trivial change (new feature, refactor, or bug
   fix). Describe what you want to do and why. This avoids wasted effort if the change
   conflicts with the project's safety or architecture goals.

2. **Security vulnerabilities** must be reported privately — see [SECURITY.md](SECURITY.md).

3. **Read the standards.** The coding rules are in:
   - `.claude/CLAUDE.md` — global C/C++ coding standard (Power of 10, MISRA C++:2023,
     F-Prime style subset)
   - `CLAUDE.md` — project-specific architecture, layering, traceability, and CI rules
   - `docs/VERIFICATION_POLICY.md` — verification methods required per classification

---

## Development workflow

### 1. Fork and branch

```bash
git clone https://github.com/<you>/messageEngine.git
cd messageEngine
git checkout -b fix/your-description   # or feat/your-description
```

### 2. Make your changes

Follow the layering rules strictly:

- `src/core/` — domain logic only; no POSIX/socket/thread calls
- `src/platform/` — OS adapters; no application policy
- `src/app/` — orchestration; uses core and platform through interfaces
- `tests/` — unit and integration tests; see permitted relaxations in `CLAUDE.md §1b`

### 3. Traceability

Every new or modified file in `src/` needs:

```cpp
// Implements: REQ-x.x[, REQ-y.y ...]
```

Every new or modified file in `tests/` needs:

```cpp
// Verifies: REQ-x.x[, REQ-y.y ...]
```

If your change adds new behaviour not covered by an existing REQ ID, propose a new
REQ ID in `CLAUDE.md §§3–7` as part of your PR.

### 4. Entry criteria — all must pass before opening a PR

```bash
make run_tests        # all tests green
make lint             # zero clang-tidy violations
make cppcheck         # zero cppcheck violations
make check_traceability  # no orphaned REQ IDs
```

Run `make static_analysis` to run lint + cppcheck + scan-build together.

> **No mbedTLS?** If your environment does not have mbedTLS installed, append `TLS=0` to every `make` command above. This excludes `TlsTcpBackend`, `TlsSessionStore`, `DtlsUdpBackend`, and `MbedtlsOpsImpl` from all targets and skips the two TLS/DTLS test binaries. PRs that touch TLS/DTLS source files must be validated with `TLS=1` (mbedTLS installed) before merge.

For changes to capacity constants or slot-management algorithms, also run:

```bash
make run_stress_tests
make run_sanitize     # ASan + UBSan
```

### 5. Safety-critical functions

If your change touches any function classified SC in `docs/HAZARD_ANALYSIS.md §3`:

- The function declaration in its `.hpp` file must carry:
  ```cpp
  // Safety-critical (SC): HAZ-NNN[, HAZ-NNN...]
  ```
- Branch coverage of all reachable branches is required. Measure it with:
  ```bash
  make coverage
  ```
  A regression on any SC function branch is a blocking defect per `CLAUDE.md §14`.
- A peer review (not self-merge) is mandatory — see `CLAUDE.md §12`.
- Update `docs/DEFECT_LOG.md` with an inspection record per `docs/INSPECTION_CHECKLIST.md`.

### 6. Open the PR

- Target branch: `main`
- Title: short imperative summary (`fix: ...`, `feat: ...`, `docs: ...`)
- Body: describe the change, reference the issue, and confirm entry criteria passed
- PRs that fail CI will not be reviewed until CI is green

---

## Code style quick reference

| Rule | Requirement |
|------|-------------|
| Language | C++17 (`-std=c++17`) |
| Exceptions | Disabled (`-fno-exceptions`) |
| RTTI | Disabled (`-fno-rtti`) |
| Dynamic allocation | Only during `init()`; never on critical paths |
| Assertions | `NEVER_COMPILED_OUT_ASSERT(cond)` — never raw `assert()` in `src/` |
| Cyclomatic complexity | ≤ 10 per function |
| Loop bounds | Statically provable upper bound required |
| Return values | Every non-void return must be checked |
| Pointer indirection | ≤ 1 level per expression; no explicit function pointers |
| STL | Forbidden in `src/`; `std::atomic<T>` for integral types is the only exception |
| Warnings | Zero — `-Wall -Wextra -Wpedantic -Werror` |

---

## Reporting bugs (non-security)

Open a [Bug Report issue](../../issues/new?template=bug_report.yml) and fill in
the template. Include the version (`ME_VERSION_STRING` from `src/core/Version.hpp`
or the git tag), the platform, and a minimal reproduction case.
