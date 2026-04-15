# UC_78 — Build with TLS=0: exclude TLS/DTLS backends via MESSAGEENGINE_NO_TLS

**HL Group:** HL-37 — TLS=0 Optional Build (MESSAGEENGINE_NO_TLS)
**Actor:** User (developer or CI system invoking `make`)
**Requirement traceability:** REQ-6.3.4 (TLS extension point)

---

## 1. Use Case Overview

- **Trigger:** User invokes `make TLS=0` (or passes `-DMESSAGEENGINE_NO_TLS` to the
  compiler directly) to build messageEngine without the mbedTLS dependency.
- **Goal:** Produce a fully functional messageEngine build that excludes all TLS/DTLS
  translation units, requires no mbedTLS headers or libraries, and passes all
  non-TLS tests. This supports environments where mbedTLS is not installed or licensed.
- **Success outcome:** Build completes with zero errors; all non-TLS targets
  (`TcpBackend`, `UdpBackend`, `LocalSimHarness`, all 22 non-TLS test binaries) are
  compiled and linked. The 2 TLS test binaries (`test_TlsTcpBackend`,
  `test_DtlsUdpBackend`) are not built or run.
- **Error outcomes:**
  - `tls_demo` and `dtls_demo` targets print an error message and exit when TLS=0.
  - Any source file that includes TLS headers outside a `#ifndef MESSAGEENGINE_NO_TLS`
    guard will fail to compile (missing mbedTLS headers).

---

## 2. Entry Points

```bash
# Makefile interface
make TLS=0                    # build everything except TLS/DTLS targets
make TLS=0 run_tests          # run all 22 non-TLS tests (skips TLS test binaries)
make TLS=0 lint               # lint all non-TLS source files
make TLS=0 cppcheck           # static analysis on non-TLS sources
make TLS=0 static_analysis    # lint + cppcheck + scan-build, TLS excluded

# Compiler preprocessor macro injected by Makefile when TLS=0
-DMESSAGEENGINE_NO_TLS
```

---

## 3. End-to-End Control Flow

**Build-time flow:**
1. `make TLS=0` — Makefile detects `TLS=0`; appends `-DMESSAGEENGINE_NO_TLS` to `CXXFLAGS`.
2. Source files guarded by `#ifndef MESSAGEENGINE_NO_TLS` are excluded from compilation:
   - `src/platform/TlsTcpBackend.cpp`
   - `src/platform/DtlsUdpBackend.cpp`
   - `src/platform/TlsSessionStore.cpp`
   - `src/platform/MbedtlsOpsImpl.cpp`
3. Test binaries `test_TlsTcpBackend` and `test_DtlsUdpBackend` are **not built** when TLS=0.
4. `tls_demo` and `dtls_demo` targets either are not built or, if invoked explicitly,
   print `"TLS not enabled in this build"` and return a non-zero exit code.
5. All remaining source files compile normally. No mbedTLS headers are needed.
6. Linking succeeds without `-lmbedtls`, `-lmbedcrypto`, `-lmbedx509`.

**Test flow with TLS=0:**
1. `make TLS=0 run_tests` — runs 22 non-TLS test binaries.
2. TLS test binaries (`test_TlsTcpBackend`, `test_DtlsUdpBackend`) are absent from the
   test target's prerequisite list and are not invoked.
3. All 22 tests pass; the build is fully green for TLS-free environments.

---

## 4. Key Components Excluded When TLS=0

| Translation unit | Type | Excluded by |
|---|---|---|
| `src/platform/TlsTcpBackend.cpp` | TLS TCP backend | `#ifndef MESSAGEENGINE_NO_TLS` |
| `src/platform/DtlsUdpBackend.cpp` | DTLS UDP backend | `#ifndef MESSAGEENGINE_NO_TLS` |
| `src/platform/TlsSessionStore.cpp` | TLS session cache | `#ifndef MESSAGEENGINE_NO_TLS` |
| `src/platform/MbedtlsOpsImpl.cpp` | mbedTLS adapter | `#ifndef MESSAGEENGINE_NO_TLS` |
| `tests/test_TlsTcpBackend.cpp` | TLS backend tests | Not built when TLS=0 |
| `tests/test_DtlsUdpBackend.cpp` | DTLS backend tests | Not built when TLS=0 |
| `tls_demo` | TLS demo app | Prints error and exits when TLS=0 |
| `dtls_demo` | DTLS demo app | Prints error and exits when TLS=0 |

---

## 5. Key Components That Remain Fully Functional When TLS=0

- `TcpBackend` — plaintext TCP; unaffected.
- `UdpBackend` — plaintext UDP; unaffected.
- `LocalSimHarness` — in-process simulation; unaffected.
- `DeliveryEngine`, `AckTracker`, `RetryManager`, `DuplicateFilter`, `Serializer` — all
  core components; unaffected.
- `ImpairmentEngine`, `PrngEngine` — impairment simulation; unaffected.
- All 22 non-TLS test binaries — fully compiled and run.

---

## 6. Branching Logic / Decision Points

This is a **compile-time** conditional, not a runtime branch. The `TLS=0` flag
controls which translation units are included in the build. There is no runtime
check or mode switch; the TLS backends simply do not exist in a TLS=0 binary.

| Condition | TLS=1 (default) | TLS=0 |
|---|---|---|
| TlsTcpBackend compiled | Yes | No |
| DtlsUdpBackend compiled | Yes | No |
| test_TlsTcpBackend built | Yes | No |
| test_DtlsUdpBackend built | Yes | No |
| tls_demo functional | Yes | Prints error, exits |
| mbedTLS link dependency | Required | Absent |
| Total test binaries | 24 | 22 |

---

## 7. Concurrency / Threading Behavior

N/A. This is a build-time selection mechanism with no runtime concurrency implications.

---

## 8. Memory & Ownership Semantics

N/A. No runtime objects are created by the TLS=0 build selection itself.

---

## 9. Error Handling Flow

- If `tls_demo` or `dtls_demo` is invoked after being built with TLS=0, the main
  function prints `"TLS not enabled in this build"` to `stderr` and returns 1.
- Any attempt to link a TLS backend object in a TLS=0 build will fail at link time
  (object files not compiled), providing clear build-time feedback.

---

## 10. External Interactions

- **mbedTLS library** — completely absent from the build dependency graph when TLS=0.
  No `-lmbedtls`, no mbedTLS headers included.
- **CI** — `make TLS=0 run_tests` is the CI command for environments without mbedTLS.
  PRs that modify TLS/DTLS source files must also be validated with `TLS=1` before merge
  (see `CONTRIBUTING.md §4`).

---

## 11. State Changes / Side Effects

None at runtime. The only effect is at build time: fewer translation units compiled,
fewer test binaries produced, no mbedTLS linkage.

---

## 12. Sequence Diagram

```
Developer -> make TLS=0 run_tests
  -> Makefile appends -DMESSAGEENGINE_NO_TLS to CXXFLAGS
  -> Compiles: TcpBackend, UdpBackend, DeliveryEngine, ... (all non-TLS sources)
  -> Skips:    TlsTcpBackend, DtlsUdpBackend, TlsSessionStore, MbedtlsOpsImpl
  -> Links:    messageEngine.a (no -lmbedtls)
  -> Builds:   22 test binaries (no test_TlsTcpBackend, no test_DtlsUdpBackend)
  -> Runs:     all 22 tests
  <- All tests green; zero mbedTLS dependency
```

---

## 13. Initialization vs Runtime Flow

N/A. TLS=0 is a build-time mechanism. There is no runtime initialization difference
between a TLS=0 build and a TLS=1 build (for the components that remain).

---

## 14. Known Risks / Observations

- **PRs touching TLS sources must be validated with TLS=1:** The TLS=0 build does not
  compile `TlsTcpBackend` or `DtlsUdpBackend`, so TLS-specific defects are invisible
  in a TLS=0 CI run. Before merging a PR that modifies TLS/DTLS source files, always
  run the full test suite with `TLS=1` (mbedTLS installed).
- **Network-accessible deployments require TLS:** A deployment without TLS in a
  network-accessible environment requires explicit documented approval (see
  `docs/STANDARDS_AND_CONFLICTS.md` Tension 5). TLS=0 is an intentional, documented
  opt-out, not a default.
- **Guard coverage:** Every source file that includes mbedTLS headers must be wrapped
  in `#ifndef MESSAGEENGINE_NO_TLS` guards. Missing guards cause build failures in
  TLS=0 environments — which is the correct fail-fast behavior.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` `make TLS=0` injects `-DMESSAGEENGINE_NO_TLS` and excludes the 4
  TLS translation units from compilation.
- `[CONFIRMED]` `test_TlsTcpBackend` and `test_DtlsUdpBackend` are not built and
  not run when TLS=0.
- `[CONFIRMED]` 22 test binaries remain (24 total minus the 2 TLS test binaries).
- `[CONFIRMED]` `tls_demo` and `dtls_demo` print an error and exit when invoked
  under a TLS=0 build.
