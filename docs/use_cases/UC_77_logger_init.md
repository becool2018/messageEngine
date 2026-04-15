# UC_77 — Logger::init(): User injects ILogClock and ILogSink before first log call

**HL Group:** HL-36 — Injectable Logger Initialization
**Actor:** User (application `main()` or test `Setup()`)
**Requirement traceability:** REQ-7.1.1, REQ-7.1.2, REQ-7.1.3, REQ-7.1.4

---

## 1. Use Case Overview

- **Trigger:** Before any `LOG_*` macro is used, User calls `Logger::init(min_level,
  clock, sink)` to supply concrete `ILogClock` and `ILogSink` implementations.
- **Goal:** Configure the Logger's timestamp source and output sink so that all
  subsequent `LOG_*` calls are routed through the correct implementations. Captures
  `getpid()` for log-line PID metadata.
- **Success outcome:** `Logger::init()` returns `Result::OK`; all subsequent `LOG_*`
  calls use the supplied clock and sink.
- **Error outcome:** Returns `Result::ERR_INVALID` if `clock` or `sink` is `nullptr`.
  In this case the Logger remains unconfigured; subsequent `LOG_*` calls fall back to a
  raw `::write(STDERR_FILENO, ...)` path.

---

## 2. Entry Points

```cpp
// src/core/Logger.hpp
static Result Logger::init(Severity min_level, ILogClock* clock, ILogSink* sink);

// Abstract injectable interfaces
// src/core/ILogClock.hpp  — virtual uint64_t mono_us(); virtual uint64_t wall_us();
// src/core/ILogSink.hpp   — virtual void write(const char* buf, uint32_t len);

// Default production implementations (src/platform/)
PosixLogClock   — clock_gettime(CLOCK_MONOTONIC) and CLOCK_REALTIME
StderrLogSink   — ::write(STDERR_FILENO, buf, len)

// Test implementations (tests/)
MockLogClock    — injected timestamps; no syscall
VectorLogSink   — accumulates log lines in a std::vector for assertion
```

---

## 3. End-to-End Control Flow

**Typical production call (in application `main()`):**
```
PosixLogClock clock;
StderrLogSink sink;
Result r = Logger::init(Severity::INFO, &clock, &sink);
// r == Result::OK; Logger is now configured
```

1. `Logger::init(min_level, clock, sink)` — entry.
2. Check: `clock == nullptr || sink == nullptr` → return `ERR_INVALID` immediately.
3. Store: `s_clock = clock`, `s_sink = sink`, `s_min_level.store(min_level)`.
4. Capture: `s_pid = static_cast<uint32_t>(::getpid())`.
5. Set: `s_initialized = true`.
6. Return `Result::OK`.

**Typical test call (in test `Setup()`):**
```
MockLogClock  mock_clock;
VectorLogSink vector_sink;
Logger::init(Severity::INFO, &mock_clock, &vector_sink);
// LOG_WARN_HI(...) output will appear in vector_sink.lines()
```

---

## 4. Call Tree

```
Logger::init(min_level, clock, sink)     [Logger.hpp / Logger.cpp]
 ├── (nullptr check)                     [guard; return ERR_INVALID on fail]
 ├── s_clock = clock                     [stores ILogClock*]
 ├── s_sink  = sink                      [stores ILogSink*]
 ├── s_min_level.store(min_level)        [std::atomic<Severity> write]
 ├── ::getpid()                          [POSIX — captures process ID]
 ├── s_pid = (uint32_t)pid               [stored for all log headers]
 └── s_initialized = true                [enables normal log path]
```

---

## 5. Key Components Involved

- **`Logger::init()`** — one-time initialization; not thread-safe. Must be called
  single-threaded before any concurrent `LOG_*` activity.
- **`ILogClock`** (`src/core/ILogClock.hpp`) — supplies `mono_us()` and `wall_us()`.
  Production: `PosixLogClock`. Tests: `MockLogClock`.
- **`ILogSink`** (`src/core/ILogSink.hpp`) — supplies `write(buf, len)`.
  Production: `StderrLogSink`. Tests: `VectorLogSink`, `MockLogSink`.
- **`s_min_level`** — `std::atomic<Severity>`; read atomically on every `LOG_*` call
  for severity filtering. Can be updated after `init()` via `Logger::set_min_level()`
  for runtime log-level changes.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `clock == nullptr \|\| sink == nullptr` | Return `ERR_INVALID`; `s_initialized` stays false | Proceed with init |

No other branching. All three stores and the `getpid()` call are unconditional.

---

## 7. Concurrency / Threading Behavior

- `Logger::init()` is **not thread-safe**. It writes `s_clock`, `s_sink`, and `s_pid`
  as plain pointer/integer assignments without a mutex.
- Call `Logger::init()` once, in a single-threaded context (e.g., `main()` before
  spawning I/O threads), before any concurrent `LOG_*` usage.
- `s_min_level` is `std::atomic<Severity>`: `set_min_level()` can be called safely
  from any thread after `init()` has returned.

---

## 8. Memory & Ownership Semantics

- `ILogClock*` and `ILogSink*` are **not owned** by the Logger. The caller retains
  ownership and must ensure both objects outlive all `LOG_*` calls (typically for the
  entire process lifetime).
- No heap allocation in `init()`. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- `ERR_INVALID` is returned when either pointer is null; no state change occurs.
- If `init()` is never called, `LOG_*` macros fall back to `::write(STDERR_FILENO, ...)`
  with the raw format string (best-effort; ensures log calls don't silently vanish).

---

## 10. External Interactions

- **`::getpid()`** — one POSIX syscall during initialization; captures process ID for
  log headers. No other syscalls in `init()`.
- No network, file, or signal interactions.

---

## 11. State Changes / Side Effects

`Logger::init()` writes the following module-level static state:

| State variable | Type | Value after init |
|---|---|---|
| `s_clock` | `ILogClock*` | caller's clock pointer |
| `s_sink` | `ILogSink*` | caller's sink pointer |
| `s_min_level` | `std::atomic<Severity>` | `min_level` argument |
| `s_pid` | `uint32_t` | current process ID from `::getpid()` |
| `s_initialized` | `bool` | `true` |

---

## 12. Sequence Diagram

```
main()
  -> Logger::init(INFO, &clock, &sink)
     -> (nullptr check passes)
     -> s_clock = &clock
     -> s_sink  = &sink
     -> s_min_level.store(INFO)
     -> getpid()  -> s_pid = N
     -> s_initialized = true
  <- Result::OK
  [LOG_*() macros now route through &clock and &sink]
```

---

## 13. Initialization vs Runtime Flow

**init() must precede runtime:** Any `LOG_*` call issued before `Logger::init()`
returns (or if `init()` returned `ERR_INVALID`) uses the fallback stderr path and
loses monotonic timestamps, severity filtering, and the configured sink.

---

## 14. Known Risks / Observations

- **Single init contract:** Calling `Logger::init()` a second time after the logger
  is already in use replaces the stored clock and sink pointers. Any in-flight
  `LOG_*` call on another thread that has already loaded the old pointer will still
  use the old sink (no memory barrier between the pointer store in `init()` and the
  load in `log()`). Avoid re-initializing while concurrent log activity is occurring.
- **Clock/sink lifetime:** If the caller's `ILogClock` or `ILogSink` object is
  destroyed before `LOG_*` calls stop, subsequent log calls will dereference a dangling
  pointer — undefined behavior. Ensure clock and sink outlive all log activity.
- **Security:** `Logger::init()` accepts any `ILogClock*` and `ILogSink*`. A compromised
  sink could exfiltrate all log output. Never accept these pointers from untrusted sources.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` `Logger::init()` is not thread-safe; it must be called once before
  any concurrent log activity.
- `[CONFIRMED]` `s_min_level` is `std::atomic<Severity>`; `set_min_level()` is safe
  to call from any thread after `init()`.
- `[CONFIRMED]` The Logger does not own the `ILogClock` or `ILogSink` pointers.
