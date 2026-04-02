# UC_46 — Logger::log() called directly by User application code to record application-level events

**HL Group:** HL-15 — Observability (Logging)
**Actor:** User
**Requirement traceability:** REQ-7.1.1, REQ-7.1.2, REQ-7.1.3, REQ-7.1.4

---

## 1. Use Case Overview

- **Trigger:** User application code calls `Logger::log(severity, component, format, ...)` to emit a structured log entry. File: `src/core/Logger.hpp`.
- **Goal:** Write a formatted log line with severity tag, component name, timestamp, and message text to `stderr` (or the configured output). The call is synchronous, non-allocating, and always returns.
- **Success outcome:** A formatted log line is written. The function returns without error.
- **Error outcomes:** None returned. If `::write()` / `fprintf` to `stderr` fails (e.g., fd closed), the failure is silently ignored to prevent logging from causing cascading failures.

---

## 2. Entry Points

```cpp
// src/core/Logger.hpp
static void Logger::log(LogSeverity severity, const char* component,
                        const char* fmt, ...);
```

---

## 3. End-to-End Control Flow

1. **`Logger::log(severity, component, fmt, ...)`** — entry (static method or free function).
2. Format the message: `vsnprintf(buf, sizeof(buf), fmt, args)` into a fixed stack buffer.
3. Prepend severity tag: `[INFO]`, `[WARNING_LO]`, `[WARNING_HI]`, or `[FATAL]`.
4. Prepend component name: `[TcpBackend]`, `[DeliveryEngine]`, etc.
5. `fprintf(stderr, "[%s][%s] %s\n", severity_str, component, buf)` (or equivalent `write(STDERR_FILENO, ...)`).
6. Returns (void).

---

## 4. Call Tree

```
Logger::log(severity, component, fmt, ...)         [Logger.hpp]
 ├── vsnprintf(buf, sizeof(buf), fmt, args)        [stack buffer format]
 └── fprintf(stderr, ...)                          [POSIX output]
```

---

## 5. Key Components Involved

- **`Logger::log()`** — the single logging entry point for the entire system; called from DeliveryEngine, TcpBackend, UdpBackend, TlsTcpBackend, DtlsUdpBackend, ImpairmentEngine, and user application code.
- **`vsnprintf()`** — format into a fixed-size stack buffer; prevents unbounded writes.
- **Severity levels:** `INFO`, `WARNING_LO`, `WARNING_HI`, `FATAL` — maps to the F-Prime severity model.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `severity == FATAL` | Log line includes [FATAL] tag; NEVER_COMPILED_OUT_ASSERT may follow | Normal log line |

No other branching. The function always attempts to write regardless of severity.

---

## 7. Concurrency / Threading Behavior

- `fprintf(stderr, ...)` is not guaranteed to be atomic for multi-line writes. In multi-threaded use, log lines from different threads may interleave.
- No `std::mutex` or `std::atomic` is used in the logger; it is designed for simplicity over thread-safety (single-threaded production deployment assumed in core use).
- For unit test scenarios with multiple threads, log interleaving is acceptable.

---

## 8. Memory & Ownership Semantics

- `buf[256]` (or similar) — stack-allocated format buffer inside `log()`.
- No heap allocation. Power of 10 Rule 3 compliant.
- `fmt` and `component` are caller-owned string literals; not copied beyond the stack buffer.

---

## 9. Error Handling Flow

- `fprintf()` / `write()` return values are not checked to avoid infinite loops (a logging failure causing a log call causing a failure...).
- If `vsnprintf()` truncates the message, the truncated version is written without error.

---

## 10. External Interactions

- **`stderr` (file descriptor 2):** Log lines are written to `stderr` by default. All system components use this same output stream.
- No network, file, or signal interactions.

---

## 11. State Changes / Side Effects

No state changes. `Logger::log()` is a pure output function with no persistent state (unless a log file or buffer is configured, which is not currently implemented in this codebase).

---

## 12. Sequence Diagram

```
User -> Logger::log(INFO, "MyApp", "Connected to %s:%u", ip, port)
  -> vsnprintf(buf, sizeof(buf), fmt, args)
  -> fprintf(stderr, "[INFO][MyApp] Connected to ...\n")
  <- (void)
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** None — `Logger::log()` may be called at any time, including before or after `DeliveryEngine::init()`.

**Runtime:** Called on every connection event, state change, error, and FATAL condition throughout the system lifetime.

---

## 14. Known Risks / Observations

- **No log level filtering:** The current implementation logs all severities unconditionally. High-volume callers (e.g., every poll iteration) should log at WARNING or higher; INFO-level logging in tight loops may impact throughput.
- **No thread-safe log sink:** Multi-threaded deployments will see interleaved log lines. This is acceptable for development/test but not for production log analysis requiring per-component ordering.
- **Payload logging prohibited:** REQ-7.1.4 prohibits logging full payload contents by default. Log calls must only include message IDs, types, sizes, and metadata.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `Logger::log()` is implemented as a variadic static method that uses `vsnprintf` into a fixed stack buffer; no heap allocation.
- `[ASSUMPTION]` Output goes to `stderr`; no configurable log sink or file output is currently implemented.
- `[ASSUMPTION]` Stack buffer size is approximately 256 bytes; messages longer than this are silently truncated.
