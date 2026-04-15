# Testing and Analysis

**Scope:** Running unit tests, stress tests, branch coverage, and static analysis for messageEngine.
**Last updated:** 2026-04-15

---

## Table of Contents

1. [Running the Tests](#1-running-the-tests)
2. [Stress Tests](#2-stress-tests)
3. [Coverage Analysis](#3-coverage-analysis)
4. [Static Analysis](#4-static-analysis)

---

## 1. Running the Tests

```bash
# Build and run all test suites
make run_tests
```

Expected output: every test prints `PASS: <test_name>` and the suite ends with `=== ALL TESTS PASSED ===`.

Individual test binaries can also be run directly:

```bash
build/test_MessageEnvelope
build/test_MessageId
build/test_Timestamp
build/test_Serializer
build/test_DuplicateFilter
build/test_AckTracker
build/test_RetryManager
build/test_Fragmentation
build/test_ReassemblyBuffer
build/test_OrderingBuffer
build/test_RequestReplyEngine
build/test_DeliveryEngine
build/test_ImpairmentEngine
build/test_ImpairmentConfigLoader
build/test_LocalSim
build/test_AssertState
build/test_SocketUtils
build/test_TcpBackend
build/test_UdpBackend
build/test_TlsTcpBackend
build/test_DtlsUdpBackend
build/test_Logger
build/test_PrngEngine
build/test_RingBuffer
```

---

## 2. Stress Tests

Six stress test binaries cover capacity-exhaustion, sustained throughput, ordering under
reorder, reconnect recovery, RingBuffer concurrent access, and TCP fan-in. They target a
different failure class than unit tests — slot leaks, index-arithmetic wrap errors, and
counter drift that only manifest under sustained load.

```bash
# Build the four main stress binaries (capacity, e2e, ordering, reconnect)
make stress_tests

# Build and run the three standard stress suites (capacity, e2e, ordering; ~60 s each)
make run_stress_tests

# Run the additional suites individually
make run_stress_reconnect   # peer reconnect / ordering-reset under load
make run_stress_ringbuffer  # RingBuffer concurrent-access storm
make run_stress_tcp_fanin   # TCP multi-client fan-in (many clients → one server)
```

**`test_stress_capacity` sub-tests** (each a function inside the capacity binary):

| Test | Cycles | What it catches |
|---|---|---|
| `test_ack_tracker_fill_drain_cycles` | 10 000 × 32 slots | Slot leak on `sweep_expired`; off-by-one at slot 33 |
| `test_ack_tracker_partial_ack_then_sweep` | 1 000 × 32 slots | Double-free if ACKed slot is re-swept; ghost entries after partial drain |
| `test_retry_manager_fill_ack_drain_cycles` | 5 000 × 32 slots | Slot not freed after `on_ack`; stale inactive entries blocking `schedule` |
| `test_retry_manager_max_retry_exhaustion` | 1 000 × 32 slots × 5 retries | Slot not freed at exhaustion; retry count exceeding `MAX_RETRY_COUNT`; backoff overflow |
| `test_ring_buffer_sustained_push_pop` | 64 000 single + 1 000 fill/drain | Index-wrap arithmetic across `uint32_t` boundary; FIFO ordering; `ERR_FULL`/`ERR_EMPTY` boundaries |
| `test_dedup_filter_window_wraparound` | 100 window lengths × 128 entries | Eviction pointer wrap; false positives after eviction; false negatives within current window |

**When to run stress tests** (per `CLAUDE.md §1c`): not required on every commit. Run when
any capacity constant in `src/core/Types.hpp` changes, or when the loop-bound or
slot-management logic in `AckTracker`, `RetryManager`, `RingBuffer`, or `DuplicateFilter`
is modified.

**CI:** Stress tests run automatically via `.github/workflows/stress.yml` on a nightly
schedule (2 AM UTC), on any push or PR to `main` that touches a capacity-relevant file,
and on manual dispatch from the GitHub Actions tab.

---

## 3. Coverage Analysis

Uses LLVM source-based coverage (`clang++ -fprofile-instr-generate -fcoverage-mapping`).

```bash
# Build instrumented binaries, run all tests, print per-file summary
make coverage

# Full report: per-file summary + per-function breakdown + policy compliance table
make coverage_report

# Annotated line-level output with branch hit counts (requires make coverage first)
make coverage_show
```

Policy floor: 100% of all reachable branches for SC files (`CLAUDE.md §14`). Per-file ceilings below 100% are documented in [`docs/COVERAGE_CEILINGS.md`](COVERAGE_CEILINGS.md); every missed branch is individually justified.

> **Methodology note:** LLVM source-based coverage counts each branch outcome (True and False) as a separate entry. All thresholds reflect LLVM output. Numbers are kept current in `docs/COVERAGE_CEILINGS.md` (single source of truth); the table below is a snapshot. All files are at their architectural maxima — see `docs/COVERAGE_CEILINGS.md` for per-file justifications.

| File | Branch % | SC? | Status |
|---|---|---|---|
| `core/Serializer.cpp` | 73.76% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/DuplicateFilter.cpp` | 73.13% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/AckTracker.cpp` | 76.97% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/RetryManager.cpp` | 77.07% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/DeliveryEngine.cpp` | ~76.2% | SC | Ceiling — assert `[[noreturn]]` branches + init-guard paths |
| `core/OrderingBuffer.cpp` | 79.33% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/Fragmentation.cpp` | 78.12% | SC | Ceiling — assert `[[noreturn]]` branches |
| `core/RequestReplyEngine.cpp` | 78.10% | SC | Ceiling — assert `[[noreturn]]` branches + payload-guard dead code |
| `core/AssertState.cpp` | 50.00% | NSC-infra | Ceiling — abort() False branch untestable |
| `platform/ImpairmentEngine.cpp` | 74.22% | SC | Ceiling — assert + unreachable branches |
| `platform/ImpairmentConfigLoader.cpp` | 83.91% | SC | Ceiling — assert + unreachable branches |
| `platform/TcpBackend.cpp` | 75.96% | SC | Ceiling — assert `[[noreturn]]` branches + hard POSIX error paths |
| `platform/UdpBackend.cpp` | 74.23% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/TlsTcpBackend.cpp` | 78.51% | SC | Ceiling — assert + hard mbedTLS/POSIX error paths |
| `platform/TlsSessionStore.cpp` | 66.67% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/DtlsUdpBackend.cpp` | 77.21% | SC | Ceiling — assert + mbedTLS/POSIX error paths |
| `platform/LocalSimHarness.cpp` | 70.49% | SC | Ceiling — assert + structurally-unreachable branches |
| `platform/MbedtlsOpsImpl.cpp` | 69.33% | SC | Ceiling — assert `[[noreturn]]` branches |
| `platform/SocketUtils.cpp` | 75.82% | NSC | Ceiling — POSIX errors unreachable on loopback |
| `platform/PosixSyscallsImpl.cpp` | 70.37% | NSC | Line coverage sufficient |
| `platform/SocketOpsImpl.cpp` | 66.67% | NSC | Line coverage sufficient |

Ceiling files are at the maximum achievable coverage: `NEVER_COMPILED_OUT_ASSERT` generates permanently-missed branch outcomes (the `[[noreturn]]` `abort()` path), and certain POSIX/mbedTLS error paths cannot be triggered in a loopback test environment. All are documented deviations, not defects. Full per-file justifications are in [`docs/COVERAGE_CEILINGS.md`](COVERAGE_CEILINGS.md).

---

## 4. Static Analysis

Three tiers of static analysis are configured in the Makefile (see [`docs/STATIC_ANALYSIS_TOOLCHAIN.md`](STATIC_ANALYSIS_TOOLCHAIN.md)):

| Tier | Tool | Status | Target |
|---|---|---|---|
| **1** | Compiler `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` | Active — enforced on every build | `make all` |
| **2a** | Clang-Tidy (strict for `src/`, relaxed for `tests/`) | Active | `make lint` |
| **2b** | Cppcheck flow/style analysis (CI-safe; no MISRA addon) | Active | `make cppcheck` |
| **2b+** | Cppcheck + MISRA C++:2023 addon (macOS only) | Active locally | `make cppcheck-misra` |
| **2c** | Clang Static Analyzer — path-sensitive, alpha checkers | Active | `make scan_build` |
| **3** | PC-lint Plus — formal MISRA C++:2023 compliance report | Pending licence | `make pclint` |

```bash
# Run all active Tier 2 tools in sequence (CI target)
make static_analysis

# Individual targets
make lint
make cppcheck
make cppcheck-misra   # macOS only — Ubuntu cppcheck 2.13 does not support --addon=misra
make scan_build
make pclint           # Tier 3 — requires PC-lint Plus licence and pclint/ config directory
```

Documented suppressions live in `.cppcheck-suppress`; each entry requires a justification comment and a `DEFECT_LOG.md` reference. On Linux, comment lines are stripped from the suppression file before passing to cppcheck (Ubuntu 24.04 cppcheck 2.13 limitation).
