# UC_46 — Caller emits a structured log line via LOG_* macros

**HL Group:** HL-15 — Observability (Logging)
**Actor:** System component (DeliveryEngine, TcpBackend, ImpairmentEngine, etc.) or User application
**Requirement traceability:** REQ-7.1.1, REQ-7.1.2, REQ-7.1.3, REQ-7.1.4

---

## 1. Use Case Overview

- **Trigger:** Source code in `src/` or a user application calls one of the `LOG_*` macros
  (e.g., `LOG_WARN_HI("AckTracker", "ack timeout mid=0x%08x", id)`) to emit a structured
  log line.
- **Goal:** Write a formatted log line containing severity, module, file:line,
  monotonic timestamp, PID, TID, and message text to the configured `ILogSink`.
  The call is synchronous, non-allocating, and always returns.
- **Success outcome:** A formatted log line is delivered to the `ILogSink`
  (e.g., `StderrLogSink` which calls `::write(STDERR_FILENO, ...)`).
- **Error outcomes:** None returned. `ILogSink::write()` failures are silently
  discarded to prevent logging from causing cascading failures.

---

## 2. Entry Points

```cpp
// src/core/Logger.hpp — preferred public interface (macros inject file:line)
LOG_INFO(module, fmt, ...)
LOG_WARN_LO(module, fmt, ...)
LOG_WARN_HI(module, fmt, ...)
LOG_FATAL(module, fmt, ...)

// Wall-clock variants (also include wall-clock timestamp field)
LOG_INFO_WALL(module, fmt, ...)
LOG_WARN_LO_WALL(module, fmt, ...)
LOG_WARN_HI_WALL(module, fmt, ...)
LOG_FATAL_WALL(module, fmt, ...)

// Underlying static method (called by macros; not called directly in src/)
static void Logger::log(Severity sev, const char* file, int line,
                        const char* module, const char* fmt, ...);
static void Logger::log_wall(Severity sev, const char* file, int line,
                             const char* module, const char* fmt, ...);

// Initialization (called once in main / test setup)
static Result Logger::init(Severity min_level, ILogClock* clock, ILogSink* sink);
```

---

## 3. End-to-End Control Flow

**Initialization (once before first log call):**
1. `Logger::init(min_level, clock, sink)` — stores `ILogClock*`, `ILogSink*`, `Severity`
   min_level; captures `getpid()` into `s_pid`. Returns `ERR_INVALID` if clock or sink
   is `nullptr`.

**Per-log-call path (via `LOG_WARN_HI("AckTracker", "ack timeout mid=0x%08x", id)`):**
1. Macro expands to `Logger::log(Severity::WARNING_HI, LOG_FILE, __LINE__, "AckTracker", "ack timeout mid=0x%08x", id)`.
2. `Logger::log()` acquires monotonic timestamp: `s_clock->mono_us()` (virtual call to
   `ILogClock` — `PosixLogClock` or test-injected mock).
3. Formats header into `buf[512]`:
   `snprintf(buf, sizeof(buf), "[%llu.%06llu][%u][%s][%u][%s][%s:%d] ", ...)`
   with mono_sec, mono_us, pid, severity_tag, tid, module, file, line.
4. `vsnprintf(buf + header_len, remaining, fmt, args)` formats caller's message body
   into the remaining portion of the same stack buffer.
5. Calls `s_sink->write(buf, total_len)` (virtual call to `ILogSink` — `StderrLogSink`
   or test-injected mock).
6. Returns (void). `write()` return value is not checked (see §9).

---

## 4. Call Tree

```
LOG_WARN_HI("module", "fmt", ...)                [Logger.hpp macro]
 └── Logger::log(WARNING_HI, file, line, ...)    [Logger.hpp / Logger.cpp]
      ├── s_clock->mono_us()                     [ILogClock vtable -> PosixLogClock]
      ├── snprintf(buf, ...)                      [header format into stack buffer]
      ├── vsnprintf(buf + hdr, ...)               [message body format]
      └── s_sink->write(buf, len)                 [ILogSink vtable -> StderrLogSink]
```

Wall-clock variant (`LOG_INFO_WALL`) additionally calls `s_clock->wall_us()` to
prepend the wall-clock timestamp field.

---

## 5. Key Components Involved

- **`LOG_*` macros** (`Logger.hpp`) — the preferred public interface; inject `__FILE__`
  and `__LINE__` automatically at the call site.
- **`Logger::log()` / `Logger::log_wall()`** — static methods backing the macros.
  Format the header + body into a fixed 512-byte stack buffer; delegate output to `ILogSink`.
- **`ILogClock`** (`src/core/ILogClock.hpp`) — abstract interface for timestamp queries
  (`mono_us()`, `wall_us()`). Default implementation: `PosixLogClock`
  (`src/platform/PosixLogClock.hpp`). Test implementation: `MockLogClock`.
- **`ILogSink`** (`src/core/ILogSink.hpp`) — abstract interface for log output
  (`write(const char* buf, uint32_t len)`). Default implementation: `StderrLogSink`
  (`src/platform/StderrLogSink.hpp`). Test implementation: `MockLogSink` / `VectorLogSink`.
- **`Logger::init()`** — one-time initialization that stores the injected clock and sink.
  Must be called before any `LOG_*` call. Returns `ERR_INVALID` if either pointer is null.
- **Severity levels:** `INFO`, `WARNING_LO`, `WARNING_HI`, `FATAL`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `sev < s_min_level` | Early return (message filtered) | Proceed with format + emit |
| `!s_initialized` | Fallback write to `STDERR_FILENO`; return | Normal path via injected sink |
| Wall-clock variant | `s_clock->wall_us()` called; wall-sec field prepended | Omitted in standard variant |

No other branching. The function always attempts to write the formatted buffer.

---

## 7. Concurrency / Threading Behavior

- `s_min_level` is `std::atomic<Severity>` — reads and writes are race-free.
- `s_clock` and `s_sink` pointers are written once during `Logger::init()` and read
  thereafter; `init()` itself is not thread-safe (must be called before any concurrent
  log activity). `s_pid` is also written once in `init()`.
- `ILogSink::write()` thread-safety depends on the implementation:
  - `StderrLogSink` uses `::write(STDERR_FILENO, ...)` which is atomic for single-syscall
    writes on POSIX (lines up to ~4 KB on Linux). Log lines from different threads may
    still interleave on long messages.
  - Test sinks (`VectorLogSink`) typically require external synchronization for
    multi-threaded tests.

---

## 8. Memory & Ownership Semantics

- `char buf[512]` — stack-allocated inside `Logger::log()`. Fixed size; no heap allocation.
  Power of 10 Rule 3 compliant.
- Header occupies at most `LOG_HEADER_MAX_SIZE` (120) bytes; caller message body gets
  at most `LOG_MSG_MAX_SIZE` (392) bytes. Messages longer than 392 bytes are silently
  truncated by `vsnprintf`.
- `fmt`, `module`, `file` — caller-owned string literals; not retained beyond the stack frame.
- `s_clock` and `s_sink` — application-owned; the `Logger` does not own or free them.

---

## 9. Error Handling Flow

- `ILogSink::write()` return value is not checked to avoid infinite error loops.
- If `vsnprintf()` truncates the message (returns >= remaining bytes), the truncated
  version is written without any error indicator.
- If `Logger::init()` has not been called (or returned `ERR_INVALID`), `Logger::log()`
  falls back to a simple `::write(STDERR_FILENO, ...)` of the raw format string to
  avoid silent data loss in improperly initialized systems.

---

## 10. External Interactions

- **`ILogSink::write()`:** The configured sink handles the actual output. For
  `StderrLogSink` this is a single `::write(STDERR_FILENO, buf, len)` syscall.
- **`ILogClock::mono_us()` / `ILogClock::wall_us()`:** The configured clock supplies
  timestamps. For `PosixLogClock` this calls `clock_gettime(CLOCK_MONOTONIC, ...)`.
- No network, file descriptor open/close, or signal interactions.

---

## 11. State Changes / Side Effects

- `Logger::init()` sets module-level static state: `s_clock`, `s_sink`, `s_min_level`,
  `s_pid`, `s_initialized`. These are write-once (or atomic in the case of `s_min_level`).
- `Logger::log()` and `Logger::log_wall()` have no persistent state changes. They are
  pure output functions that format and forward to the sink.

---

## 12. Sequence Diagram

```
Caller -> LOG_WARN_HI("AckTracker", "ack timeout mid=0x%08x", id)
  -> Logger::log(WARNING_HI, __FILE__, __LINE__, "AckTracker", ...)
    -> s_clock->mono_us()                           [ILogClock]
    -> snprintf(buf, ...)                           [header]
    -> vsnprintf(buf + hdr, ...)                    [message body]
    -> s_sink->write(buf, total_len)                [ILogSink -> StderrLogSink]
  <- (void)
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** `Logger::init(min_level, clock, sink)` must be called once before
any `LOG_*` macro is used. Typical call site: application `main()` or test `Setup()`.

**Runtime:** Called on every connection event, state change, error, and FATAL condition
throughout the system lifetime.

---

## 14. Known Risks / Observations

- **Payload logging prohibited:** REQ-7.1.4 prohibits logging full payload contents
  by default. All `LOG_*` call sites must log message IDs, types, sizes, and metadata
  only — never raw payload bytes.
- **`init()` not thread-safe:** `Logger::init()` writes `s_clock`, `s_sink`, and
  `s_pid` non-atomically. Call it once, single-threaded, before starting any
  concurrent log activity.
- **`s_min_level` is atomic:** Run-time severity filtering via `Logger::set_min_level()`
  is thread-safe; the race-free read in `log()` allows dynamic log-level changes without
  a mutex.
- **Message truncation at 392 bytes:** The stack-buffer maximum for the caller's message
  body is `LOG_MSG_MAX_SIZE` (392 bytes). Long diagnostic messages will be silently
  truncated.
- **Clock/sink must be trusted:** `Logger::init()` accepts any `ILogClock*` and
  `ILogSink*` without integrity verification. A malicious or compromised sink could
  exfiltrate all log output. Never accept these pointers from untrusted sources.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` The `LOG_*` macros are the public interface; `Logger::log()` is
  internal and not called directly in `src/` production code.
- `[CONFIRMED]` Stack buffer is 512 bytes; header consumes up to 120 bytes;
  caller message body gets up to 392 bytes.
- `[CONFIRMED]` Output goes through the injected `ILogSink`; `StderrLogSink` is
  the default production sink; test sinks (`VectorLogSink`, `MockLogSink`) are used
  in unit tests.
- `[CONFIRMED]` `ILogClock` provides `mono_us()` and `wall_us()`; `PosixLogClock`
  is the default production clock backed by `clock_gettime(CLOCK_MONOTONIC, ...)`.
