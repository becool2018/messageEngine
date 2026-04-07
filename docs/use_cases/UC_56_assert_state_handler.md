# UC_56 — AssertState / IResetHandler / AbortResetHandler: assertion-failure flag and handler dispatch

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-3.2.1 (indirect — assertion infrastructure supports all safety-critical REQs)

This is a System Internal use case. The assertion infrastructure (`Assert.hpp`, `AssertState.hpp`, `IResetHandler.hpp`, `AbortResetHandler.hpp`) is triggered by `NEVER_COMPILED_OUT_ASSERT(cond)` when `cond` is false. The User never calls it directly; they register a handler via `assert_state::set_reset_handler()` during application initialization.

---

## 1. Use Case Overview

- **Trigger:** `NEVER_COMPILED_OUT_ASSERT(cond)` is evaluated and `cond` is false at runtime. Macro defined in `src/core/Assert.hpp`.
- **Goal:** Log the condition text, file, and line number at FATAL severity, then invoke the registered `IResetHandler::on_fatal_assert()` for controlled component reset. In test/debug builds, call `::abort()` for immediate crash with stack trace.
- **Success outcome (production):** `IResetHandler::on_fatal_assert()` is called; component is reset; `g_fatal_fired` is set to true.
- **Success outcome (debug/test):** `::abort()` is called; process terminates with core dump / stack trace.
- **Error outcomes:** If no handler is registered, `::abort()` is called as fallback.

---

## 2. Entry Points

```cpp
// src/core/Assert.hpp
NEVER_COMPILED_OUT_ASSERT(cond)                    [macro]

// src/core/AssertState.hpp
namespace assert_state {
    void set_reset_handler(IResetHandler* handler);
    bool fatal_fired();
}

// src/core/IResetHandler.hpp
class IResetHandler {
    virtual void on_fatal_assert(const char* cond, const char* file, int line) = 0;
};

// src/core/AbortResetHandler.hpp
class AbortResetHandler : public IResetHandler {
    void on_fatal_assert(const char* cond, const char* file, int line) override;  // calls ::abort()
    static AbortResetHandler& instance();
};
```

---

## 3. End-to-End Control Flow

**`NEVER_COMPILED_OUT_ASSERT(cond)` when `cond == false`:**
1. `Logger::log(FATAL, "Assert", "Assertion failed: %s at %s:%d", #cond, __FILE__, __LINE__)`.
2. In debug/test build: `::abort()` — immediate crash, no handler dispatch.
3. In production build:
   a. `g_fatal_fired = true` (or `assert_state::set_fatal_fired()`).
   b. `IResetHandler* h = assert_state::get_reset_handler()`.
   c. If `h != nullptr`: `h->on_fatal_assert(#cond, __FILE__, __LINE__)` — virtual dispatch to registered handler, passing condition text, file name, and line number.
   d. If `h == nullptr` or `on_fatal_assert()` returns: `::abort()` as fallback.

**`assert_state::set_reset_handler(handler)`:**
1. Stores `handler` pointer in a module-static variable.
2. Thread-safe: typically called once during application init before any component runs.

**`AbortResetHandler::on_fatal_assert()`:**
1. Calls `::abort()` — immediate process termination.
2. Used in POSIX builds per `CLAUDE.md §10.2`: "For POSIX builds register AbortResetHandler::instance()".

---

## 4. Call Tree

```
NEVER_COMPILED_OUT_ASSERT(cond)                    [Assert.hpp macro]
 ├── Logger::log(FATAL, ...)
 └── [production path]
      ├── assert_state::set_fatal_fired()
      ├── assert_state::get_reset_handler()
      └── IResetHandler::on_fatal_assert(cond, file, line)  [virtual dispatch]
           └── AbortResetHandler::on_fatal_assert(cond, file, line) -> ::abort()
```

---

## 5. Key Components Involved

- **`NEVER_COMPILED_OUT_ASSERT`** — never disabled by `NDEBUG`; complies with JPL Power of 10 Rule 5 (assertions in all builds).
- **`IResetHandler`** — virtual interface; production code uses vtable dispatch (Rule 9 exception for virtual functions).
- **`AbortResetHandler`** — POSIX implementation; calls `::abort()`. Singleton accessed via `instance()`.
- **`g_fatal_fired`** — module-static flag; set on any assert failure; allows caller to check if a reset was triggered.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `cond == true` | No action (assert passes) | Trigger failure path |
| Debug/test build | `::abort()` immediately | Production handler dispatch |
| Handler registered | `on_fatal_assert()` via vtable | `::abort()` fallback |
| `on_fatal_assert()` returns | `::abort()` fallback | (embedded: soft reset, no abort) |

---

## 7. Concurrency / Threading Behavior

- `NEVER_COMPILED_OUT_ASSERT` may fire from any thread.
- `Logger::log()` and `::abort()` are the only operations after the assert fires; thread-safety is irrelevant once abort is called.
- `set_reset_handler()` must be called before any component starts; no concurrent access expected.
- No `std::atomic` operations (the fatal flag may be a plain bool set once on the abort path).

---

## 8. Memory & Ownership Semantics

- `g_fatal_fired` — module-static bool; no heap allocation.
- The registered `IResetHandler*` pointer is stored in a module-static variable; no ownership transfer.
- No heap allocation on the assert path. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- An assert failure IS the unrecoverable error. The assert path is terminal.
- The only "recovery" is the embedded soft-reset path via `IResetHandler::on_fatal_assert()`, which is a controlled restart of the affected component.

---

## 10. External Interactions

- **`::abort()`** — POSIX signal `SIGABRT`; terminates the process with a core dump on debug builds.
- **`stderr`** — FATAL log written via `Logger::log()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `assert_state` | `g_fatal_fired` | false | true |
| Process | alive | running | terminated (SIGABRT) or reset |

---

## 12. Sequence Diagram

```
[Component: NEVER_COMPILED_OUT_ASSERT(m_open) fails]
  -> Logger::log(FATAL, "Assert", "Assertion failed: m_open at TcpBackend.cpp:42")
  -> assert_state::set_fatal_fired()
  -> assert_state::get_reset_handler()         <- AbortResetHandler::instance()
  -> AbortResetHandler::on_fatal_assert()
       -> ::abort()
  [Process terminates / core dump]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `assert_state::set_reset_handler(&AbortResetHandler::instance())` must be called in `main()` or test setup before any component is initialized.

**Runtime:**
- `NEVER_COMPILED_OUT_ASSERT(cond)` evaluated on every function entry where preconditions are checked. Failure is always terminal in POSIX builds.

---

## 14. Known Risks / Observations

- **No-handler fallback:** If `set_reset_handler()` is not called, the fallback is `::abort()`. This is safe but produces a less informative abort (no soft-reset path).
- **`g_fatal_fired` use in tests:** Test code can check `assert_state::fatal_fired()` after a test scenario to verify that an expected assert fired. This is used for negative testing.
- **Async-signal safety:** `Logger::log()` uses `fprintf(stderr, ...)`; `fprintf` is not async-signal-safe. If `NEVER_COMPILED_OUT_ASSERT` fires in a signal handler context, the log call has undefined behavior. This is acceptable given that signal-handler assertions are not a design use case.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `NEVER_COMPILED_OUT_ASSERT` logs before calling the reset handler; the exact macro expansion is inferred from `Assert.hpp` documentation in `CLAUDE.md §10`.
- `[ASSUMPTION]` `g_fatal_fired` is a module-static `bool` (or `std::atomic<bool>`) in `AssertState.cpp`; exact implementation is inferred.
- `[ASSUMPTION]` `AbortResetHandler::instance()` returns a reference to a static singleton; no heap allocation.
