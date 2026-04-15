# Logging in messageEngine

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Log Format](#2-log-format)
3. [Architecture Overview](#3-architecture-overview)
4. [Interfaces](#4-interfaces)
5. [Initialization](#5-initialization)
6. [Macros Reference](#6-macros-reference)
7. [Severity Levels and Verbosity](#7-severity-levels-and-verbosity)
8. [Buffer Sizing](#8-buffer-sizing)
9. [Extending the Logger](#9-extending-the-logger)
   - [Custom sink (write to file, network, ring buffer)](#91-custom-sink)
   - [Custom clock (simulated time, monotonic override)](#92-custom-clock)
   - [Test doubles (fault injection)](#93-test-doubles)
10. [Using the Logger in Tests](#10-using-the-logger-in-tests)
11. [Thread Safety](#11-thread-safety)
12. [Design Constraints and Deviations](#12-design-constraints-and-deviations)
13. [File Map](#13-file-map)

---

## 1. Quick Start

```cpp
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"

// Call once before any logging — typically in main() or your init path.
Result r = Logger::init(Severity::INFO,
                        &PosixLogClock::instance(),
                        &PosixLogSink::instance());

// Then log from anywhere using the macros:
LOG_INFO("MyModule", "connected peer=%u slot=%u", peer_id, slot);
LOG_WARN_LO("RetryMgr", "retry %u of %u mid=0x%08x", attempt, max, msg_id);
LOG_WARN_HI("Transport", "send failed fd=%d err=%d", fd, errno);
LOG_FATAL("Serializer", "buffer overflow len=%u max=%u", len, max_len);

// Wall-clock variant (prepends absolute timestamp before the monotonic one):
LOG_INFO_WALL("TcpBackend", "session established peer=%u", id);
```

Output goes to **stderr** via a single `write(2)` call per log line. No heap allocation. No stdio buffering.

---

## 2. Log Format

### Standard macros (`LOG_INFO`, `LOG_WARN_LO`, `LOG_WARN_HI`, `LOG_FATAL`)

```
[mono_sec.mono_us][pid][severity][tid][module][file:line] message
```

Example:
```
[0000042.123456][1234][INFO    ][3891273984][TcpBackend][TcpBackend.cpp:611] Server listening on 127.0.0.1:58244
```

| Field | Description |
|-------|-------------|
| `mono_sec.mono_us` | `CLOCK_MONOTONIC` — seconds since system boot, six-digit microsecond fraction. Zero-drifts across reboots; safe for latency measurement between log lines. |
| `pid` | Process ID, captured once at `Logger::init()` — zero per-call cost. |
| `severity` | Fixed-width 8-character tag: `INFO    `, `WARN_LO `, `WARN_HI `, `FATAL   `. Fixed width keeps columns aligned when grepping. |
| `tid` | Thread ID from `pthread_self()`, cast to `uint32_t`. Lets you separate interleaved output from concurrent threads. |
| `module` | Caller-supplied string identifying the component (e.g. `"TcpBackend"`, `"AckTracker"`). |
| `file:line` | Basename of source file + line number, injected by the macro at compile time via `__builtin_strrchr(__FILE__, '/')`. Zero runtime cost. |
| `message` | `printf`-style body. Format string is validated at compile time by `__attribute__((format(printf, 5, 6)))`. |

### Wall-clock variants (`LOG_INFO_WALL`, `LOG_WARN_LO_WALL`, `LOG_WARN_HI_WALL`, `LOG_FATAL_WALL`)

```
[wall_sec.wall_us][mono_sec.mono_us][pid][severity][tid][module][file:line] message
```

The wall timestamp (`CLOCK_REALTIME`) is **prepended** before the monotonic one. Use these when you need to correlate a log line with an external system clock — for example, at connection establishment or a fatal event. For all routine logging prefer the standard monotonic-only variants: they are immune to `settimeofday` and NTP jumps.

Note: the `wall_sec` field is formatted with `%llu` (no zero-padding), so its width grows with the epoch value (e.g. `1744900000` today). The `mono_sec` field is zero-padded to 7 digits (`%07llu`). The two fields therefore have different visual widths in mixed-format output.

---

## 3. Architecture Overview

```
        ┌─────────────────────────────────────────────────┐
        │              Caller (any module)                │
        │  LOG_INFO("M", "fmt", ...)                      │
        └───────────────────┬─────────────────────────────┘
                            │ expands to Logger::log(Severity::INFO,
                            │   LOG_FILE, __LINE__, "M", "fmt", ...)
                            ▼
        ┌─────────────────────────────────────────────────┐
        │              src/core/Logger.cpp                │
        │                                                 │
        │  1. severity filter (sev < s_min_level → drop)  │
        │  2. s_clock->now_monotonic_us()                 │
        │  3. s_clock->thread_id()                        │
        │  4. snprintf header into buf[512]               │
        │  5. vsnprintf message body into buf+hdr         │
        │  6. append '\n'                                 │
        │  7. s_sink->write(buf, len)                     │
        └──────────┬────────────────────┬─────────────────┘
                   │                    │
        ┌──────────▼──────┐   ┌─────────▼──────────┐
        │  ILogClock      │   │  ILogSink           │
        │  (src/core/)    │   │  (src/core/)        │
        └──────────┬──────┘   └─────────┬───────────┘
                   │                    │
        ┌──────────▼──────┐   ┌─────────▼───────────┐
        │ PosixLogClock   │   │ PosixLogSink         │
        │ (src/platform/) │   │ (src/platform/)      │
        │                 │   │                      │
        │ clock_gettime() │   │ write(2) → stderr    │
        │ pthread_self()  │   │                      │
        └─────────────────┘   └──────────────────────┘
```

**Key design properties:**

- `src/core/Logger.cpp` contains **no POSIX headers**. All OS calls are behind the `ILogClock` and `ILogSink` interfaces, which live in `src/core/`. The POSIX implementations live in `src/platform/` — the only place in the codebase that may call `clock_gettime`, `pthread_self`, or `write(2)`.
- The 512-byte format buffer is **stack-allocated** inside `Logger::log()`. No heap is touched after `Logger::init()`.
- Each log line is written with a **single `write(2)` call**, making lines atomic for line lengths below `PIPE_BUF` (~4096 bytes on Linux/macOS).

---

## 4. Interfaces

### `ILogClock` — `src/core/ILogClock.hpp`

```cpp
class ILogClock {
public:
    virtual ~ILogClock() = default;
    virtual uint64_t now_wall_us()      const = 0;  // CLOCK_REALTIME, microseconds
    virtual uint64_t now_monotonic_us() const = 0;  // CLOCK_MONOTONIC, microseconds
    virtual uint32_t thread_id()        const = 0;  // platform thread ID
};
```

### `ILogSink` — `src/core/ILogSink.hpp`

```cpp
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const char* buf, uint32_t len) = 0;
};
```

Both are pure-virtual interfaces. `Logger` stores pointers to them after `init()` and calls through them on every log line. You can substitute any implementation that satisfies the interface — the Logger never knows or cares whether it is talking to stderr, a file, a ring buffer, or a test spy.

---

## 5. Initialization

```cpp
Result Logger::init(Severity min_level, ILogClock* clock, ILogSink* sink);
```

Call **once** before any logging, typically at the start of `main()` or your component's `init()`:

```cpp
Result r = Logger::init(Severity::WARNING_LO,
                        &PosixLogClock::instance(),
                        &PosixLogSink::instance());
if (r != Result::OK) {
    // clock or sink was null — cannot proceed
}
```

- `min_level` — messages with severity strictly below this level are dropped before any formatting work is done. See [§7](#7-severity-levels-and-verbosity).
- `clock` — must be non-null; returns `ERR_INVALID` otherwise.
- `sink` — must be non-null; returns `ERR_INVALID` otherwise.
- Captures `getpid()` once into a static variable — zero per-call PID overhead.
- The min_level can only be changed by calling `init()` again. There is no runtime `set_level()` API (see [§7](#7-severity-levels-and-verbosity) for the rationale and workaround).

---

## 6. Macros Reference

All macros inject `__FILE__` (basename only) and `__LINE__` at the call site at compile time.

### Standard (monotonic timestamp only)

| Macro | Severity | Typical use |
|-------|----------|-------------|
| `LOG_INFO(mod, fmt, ...)` | `INFO` | Normal operational events: connections, state changes, periodic status |
| `LOG_WARN_LO(mod, fmt, ...)` | `WARNING_LO` | Localized recoverable issues: retry fired, queue full, short read |
| `LOG_WARN_HI(mod, fmt, ...)` | `WARNING_HI` | System-wide recoverable issues: peer misbehaving, security policy violation |
| `LOG_FATAL(mod, fmt, ...)` | `FATAL` | Unrecoverable failures that require component restart |

### Wall-clock variants (wall + monotonic timestamps)

| Macro | Severity |
|-------|----------|
| `LOG_INFO_WALL(mod, fmt, ...)` | `INFO` |
| `LOG_WARN_LO_WALL(mod, fmt, ...)` | `WARNING_LO` |
| `LOG_WARN_HI_WALL(mod, fmt, ...)` | `WARNING_HI` |
| `LOG_FATAL_WALL(mod, fmt, ...)` | `FATAL` |

Use the `_WALL` variants sparingly — only when the absolute wall-clock time is meaningful to an external observer (e.g. connection establishment, partition events, fatal shutdown). The extra field adds ~20 bytes to the header and makes lines harder to grep in bulk.

### Message context convention

Embed message-specific context as short `key=value` pairs directly in the format string. This makes lines self-contained and grep-friendly:

```cpp
LOG_INFO("DeliveryEngine", "send ok mid=0x%08x peer=%u chan=%u", mid, peer, chan);
LOG_WARN_HI("AckTracker",  "ack timeout mid=0x%08x retry=%u", mid, retry);
LOG_INFO("TcpBackend",     "connected peer=%u slot=%u fd=%d", peer, slot, fd);
```

Cross-component correlation is then a single `grep`:
```sh
grep 'mid=0x000012ab' app.log   # traces one message through send → impair → receive
```

---

## 7. Severity Levels and Verbosity

The four levels in ascending order:

| Level | Value | Meaning |
|-------|-------|---------|
| `Severity::INFO` | 0 | Normal operational events |
| `Severity::WARNING_LO` | 1 | Localized, recoverable |
| `Severity::WARNING_HI` | 2 | System-wide, recoverable |
| `Severity::FATAL` | 3 | Unrecoverable — triggers reset handler |

**Choosing `min_level` at init:**

```cpp
// Production: suppress INFO, show warnings and fatals only
Logger::init(Severity::WARNING_LO, &clock, &sink);

// Development / debug: show everything
Logger::init(Severity::INFO, &clock, &sink);

// Quiet mode: show only fatal events
Logger::init(Severity::FATAL, &clock, &sink);
```

**Runtime level changes:** There is no `Logger::set_level()` API. The `s_min_level` static is declared as `std::atomic<Severity>`, so reads in concurrent `log()` calls are safe. However, `Logger::init()` as a whole is not thread-safe (it also writes `s_clock`, `s_sink`, and `s_pid` non-atomically), so changing the level by calling `init()` again must still be done during a quiescent phase.

**Workaround for now:** If you need to dynamically change verbosity, call `Logger::init()` again with the new level. This is safe as long as no other thread is concurrently inside `Logger::log()` at that moment (i.e. do it during a quiescent phase of your application).

---

## 8. Buffer Sizing

The format buffer is a fixed 512-byte stack array in `Logger::log()` and `Logger::log_wall()`. The constants are defined as `static constexpr` members of `Logger`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `LOG_FORMAT_BUF_SIZE` | 512 | Total buffer: header + message body |
| `LOG_HEADER_MAX_SIZE` | 120 | Reserved for the standard header |
| `LOG_HEADER_WALL_MAX_SIZE` | 140 | Reserved for the wall-clock header |
| `LOG_MSG_MAX_SIZE` | 392 | Minimum space for message body (standard) |
| `LOG_MSG_WALL_MAX_SIZE` | 372 | Minimum space for message body (wall variant) |

If the header + body together exceed 511 characters, `vsnprintf` silently truncates the body at the buffer boundary. The newline is always appended and the line is always written — you will never get a partial line or a missing newline. You will simply lose the tail of an unusually long message.

**To increase the buffer:** Change `LOG_FORMAT_BUF_SIZE` in `Logger.hpp`. The `static_assert` guards in `Logger.cpp` will catch any constant relationship violations at compile time. Also update `docs/STACK_ANALYSIS.md` per CLAUDE.md §15 — the buffer lives on the stack in `Logger::log()`'s frame.

---

## 9. Extending the Logger

### 9.1 Custom sink

Implement `ILogSink` to redirect log output anywhere — a file, a ring buffer, a UDP socket, a test spy:

```cpp
// Example: write to a file descriptor instead of stderr
class FdLogSink : public ILogSink {
public:
    explicit FdLogSink(int fd) : m_fd(fd) {}
    ~FdLogSink() override = default;

    void write(const char* buf, uint32_t len) override {
        (void)::write(m_fd, buf, static_cast<size_t>(len));
        // Partial/failed writes are silently dropped — same policy as PosixLogSink.
    }

private:
    int m_fd;
};

// Usage:
int log_fd = open("app.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
FdLogSink file_sink(log_fd);
Logger::init(Severity::INFO, &PosixLogClock::instance(), &file_sink);
```

**Rules:**
- `write()` must not allocate heap memory (Power of 10 Rule 3 on the logging path).
- `write()` must not call any `LOG_*` macro (would recurse into Logger with `s_sink` already engaged).
- `write()` may be called from multiple threads concurrently; make it thread-safe if needed.

### 9.2 Custom clock

Implement `ILogClock` to use a different time source — simulated time in a deterministic test harness, a hardware RTC, or a GPS-disciplined clock:

```cpp
class SimClock : public ILogClock {
public:
    void set_mono(uint64_t us) { m_mono_us = us; }
    void set_wall(uint64_t us) { m_wall_us = us; }

    uint64_t now_wall_us()      const override { return m_wall_us; }
    uint64_t now_monotonic_us() const override { return m_mono_us; }
    uint32_t thread_id()        const override { return 1U; }

private:
    uint64_t m_mono_us = 0U;
    uint64_t m_wall_us = 0U;
};

// Usage:
SimClock sim_clock;
sim_clock.set_mono(500000U);  // t = 0.5 seconds
Logger::init(Severity::INFO, &sim_clock, &PosixLogSink::instance());
```

### 9.3 Test doubles

The production POSIX implementations (`PosixLogClock`, `PosixLogSink`) use the **Non-Virtual Interface (NVI)** pattern to expose a protected virtual seam for fault injection without modifying production code:

**Injecting a `clock_gettime` failure:**

```cpp
// tests/FaultPosixLogClock.hpp
class FaultPosixLogClock : public PosixLogClock {
public:
    static FaultPosixLogClock& instance() {
        static FaultPosixLogClock inst;
        return inst;
    }
protected:
    int do_clock_gettime(clockid_t, struct timespec* ts) const override {
        ts->tv_sec = 0; ts->tv_nsec = 0;
        return -1;  // simulate clock failure
    }
};
```

**Injecting a `write(2)` failure:**

```cpp
// tests/FaultPosixLogSink.hpp
class FaultPosixLogSink : public PosixLogSink {
public:
    explicit FaultPosixLogSink(ssize_t ret) : m_ret(ret) {}
protected:
    ssize_t do_write(int, const void*, size_t) override {
        return m_ret;  // e.g. 0 = partial write, -1 = error
    }
private:
    ssize_t m_ret;
};
```

These are the same test doubles used in `tests/test_Logger.cpp` for the M5 fault-injection tests (T-4.x). See `tests/FaultPosixLogClock.hpp` and `tests/FaultPosixLogSink.hpp` for the full implementations.

**Capturing log output in tests:**

`tests/RingLogSink.hpp` provides a fixed-capacity ring buffer that captures formatted log lines for assertion in tests:

```cpp
RingLogSink<32> sink;  // capture up to 32 lines
Logger::init(Severity::INFO, &FakeLogClock::instance(), &sink);

LOG_INFO("MyModule", "hello world");

// Inspect what was written:
const char* line = sink.line(0);  // "[0000000.000000][0][INFO    ]..."
assert(strstr(line, "hello world") != nullptr);
```

---

## 10. Using the Logger in Tests

**Preferred pattern:** inject a `FakeLogClock` and `RingLogSink`. This keeps test output deterministic (no real timestamps, no stderr noise) and lets you assert on log content.

```cpp
FakeLogClock  clock;
RingLogSink<64> sink;
Logger::init(Severity::INFO, &clock, &sink);

// Exercise the component under test — its LOG_* calls go into sink.
my_component.do_something();

// Assert on log output if relevant to the test:
assert(sink.count() > 0);
assert(strstr(sink.line(0), "expected substring") != nullptr);
```

**Important:** `Logger` holds static state (`s_clock`, `s_sink`, `s_min_level`). If multiple tests run in the same process and each calls `Logger::init()` with a different sink, the last init wins. Structure your tests so each test either re-inits the logger at the top or shares a single sink for the whole suite.

---

## 11. Thread Safety

| Operation | Thread-safe? | Notes |
|-----------|-------------|-------|
| `Logger::log()` / `Logger::log_wall()` | **Yes** — reads only | All per-call state is on the stack. `s_clock` and `s_sink` are read-only after `init()`. The stack buffer is per-call, per-frame. |
| `Logger::init()` | **No — not concurrent** | Writes to `s_min_level`, `s_clock`, `s_sink`, `s_pid`. Must be called before any concurrent logging begins, or during a quiescent phase. |
| `PosixLogSink::write()` | **Yes — atomic at OS level** | Single `write(2)` call per line. Lines up to `PIPE_BUF` bytes (~4096 on Linux/macOS) are written atomically by the kernel. Lines above `PIPE_BUF` may interleave with other writers. With `LOG_FORMAT_BUF_SIZE = 512`, all lines are well below `PIPE_BUF`. |
| `PosixLogClock::now_*()` | **Yes** | `clock_gettime` and `pthread_self` are both thread-safe POSIX calls. |

---

## 12. Design Constraints and Deviations

| Constraint | Rule | Impact |
|------------|------|--------|
| No heap after init | Power of 10 Rule 3 | Format buffer is stack-allocated (512 bytes in `Logger::log()`'s frame). |
| No STL | F-Prime / MISRA | No `std::string`, `std::ostringstream`, etc. `printf`-style formatting only. |
| No exceptions | `-fno-exceptions` | Errors (null clock/sink) returned via `Result`; format errors drop the line silently. |
| No function pointers | Power of 10 Rule 9 | `ILogClock` and `ILogSink` dispatch via vtable (approved exception to Rule 9). |
| `__attribute__((format))` | GCC/Clang extension | Enables compile-time format-string validation. MISRA C++:2023 Rule 19.3.4 deviation, documented in `Logger.hpp`. |
| `##__VA_ARGS__` | GCC/Clang extension | Suppresses trailing comma when no variadic args are supplied. Same MISRA deviation, same documentation. |
| `__builtin_strrchr` | GCC/Clang built-in | Compile-time basename stripping in `LOG_FILE` macro. Zero runtime cost; portability to MSVC not required. |
| `final` omitted on `PosixLogClock` / `PosixLogSink` | Deliberate deviation | Required to allow NVI test-seam subclassing (`FaultPosixLogClock`, `FaultPosixLogSink`). Documented in both headers. |

---

## 13. File Map

| File | Role |
|------|------|
| `src/core/ILogSink.hpp` | Pure-virtual sink interface (no POSIX) |
| `src/core/ILogClock.hpp` | Pure-virtual clock + TID interface (no POSIX) |
| `src/core/Logger.hpp` | Logger class declaration + all `LOG_*` macros |
| `src/core/Logger.cpp` | `init()`, `log()`, `log_wall()`, `severity_tag()` |
| `src/platform/PosixLogSink.hpp` | POSIX sink: `write(2)` to stderr, NVI seam |
| `src/platform/PosixLogSink.cpp` | `write()`, `do_write()`, `instance()` |
| `src/platform/PosixLogClock.hpp` | POSIX clock: `clock_gettime` + `pthread_self`, NVI seam |
| `src/platform/PosixLogClock.cpp` | `now_wall_us()`, `now_monotonic_us()`, `thread_id()`, `instance()` |
| `tests/test_Logger.cpp` | 24-test suite, T-1 through T-6, M1+M2+M4+M5 |
| `tests/FakeLogClock.hpp` | Settable-value `ILogClock` for unit tests |
| `tests/RingLogSink.hpp` | Fixed-capacity capture sink for output assertion |
| `tests/FaultPosixLogClock.hpp` | NVI fault-injection: simulates `clock_gettime` failure |
| `tests/FaultPosixLogSink.hpp` | NVI fault-injection: simulates `write(2)` failure |
| `docs/COVERAGE_CEILINGS.md` | Branch coverage thresholds and ceiling arguments for Logger files |
| `docs/STACK_ANALYSIS.md` | Stack depth analysis including Logger call chain |
