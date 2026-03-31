# UC_41 — impairment_config_load(): read file, parse all recognised keys, clamp probabilities, return populated ImpairmentConfig

**HL Group:** HL-20 — Load Impairment Config from File
**Actor:** User
**Requirement traceability:** REQ-5.2.1

---

## 1. Use Case Overview

**Trigger:** User calls `impairment_config_load(path, cfg)` with a path to an INI-style key=value text file and a reference to an `ImpairmentConfig` struct to populate.

**Goal:** Open the file, read at most `MAX_CONFIG_LINES` (64) lines, parse each recognised key=value pair, clamp probability values to [0.0, 1.0], and return `Result::OK` with `cfg` populated. Unknown keys are logged and skipped; missing keys retain safe defaults. The struct is always initialised to defaults before any parsing begins, so even a failed open leaves `cfg` in a well-defined state.

**Success outcome:** Returns `Result::OK`. `cfg` reflects any values specified in the file merged over the safe defaults from `impairment_config_default()`. Postconditions on `loss_probability` and `duplication_probability` are asserted to be in [0.0, 1.0].

**Error outcomes:**
- `fopen()` fails (file not found, permission denied) → returns `Result::ERR_IO`; `cfg` contains defaults.
- File has more than 64 lines → lines beyond line 64 are silently ignored; first 64 lines are still parsed; returns `Result::OK`.
- Malformed line (cannot extract key and value with `sscanf`) → line is logged at WARNING_LO and skipped; parsing continues.
- Unknown key → logged at WARNING_LO and skipped.
- `fclose()` returns non-zero → logged at WARNING_LO; still returns `Result::OK`.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentConfigLoader.cpp, line 170
// Safety-critical (SC): HAZ-002, HAZ-007 — verified to M5
Result impairment_config_load(const char* path, ImpairmentConfig& cfg)
```

Declared in:
```
// src/platform/ImpairmentConfigLoader.hpp
```

---

## 3. End-to-End Control Flow (Step-by-Step)

1. `impairment_config_load(path, cfg)` called. Assertion: `path != nullptr`.
2. `impairment_config_default(cfg)` — sets all fields to safe defaults: `enabled = false`, all latency/jitter/loss/duplication values zeroed, `prng_seed = 42`. This ensures `cfg` is well-defined even if `fopen()` fails immediately.
3. `FILE* fp = fopen(path, "r")`. If `fp == nullptr`: log WARNING_LO "Cannot open impairment config file: <path>"; return `Result::ERR_IO`. `cfg` retains the defaults set in step 2.
4. INFO log: "Loading impairment config: <path>".
5. Declare `char line[MAX_CONFIG_LINE_LEN]` (128 bytes, stack-allocated). Declare `bool eof_reached = false`.
6. Enter the bounded `for` loop: `i = 0U; i < MAX_CONFIG_LINES; ++i` (at most 64 iterations; Power of 10 Rule 2).
   a. `const char* got = fgets(line, MAX_CONFIG_LINE_LEN, fp)`.
   b. If `got == nullptr` (EOF or error): `eof_reached = true`; `break`.
   c. Otherwise: `parse_config_line(line, cfg)`.
7. After the loop: if `!eof_reached` (the file had more than 64 lines): log WARNING_LO "Config file has more than 64 lines; remaining lines ignored".
8. `int close_res = fclose(fp)`. If `close_res != 0`: log WARNING_LO "fclose() returned non-zero for: <path>".
9. Postcondition assertions:
   - `cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0`.
   - `cfg.duplication_probability >= 0.0 && cfg.duplication_probability <= 1.0`.
10. INFO log summary: "Impairment config loaded: enabled=N latency=N loss=N.NNN seed=N".
11. Return `Result::OK`.

**parse_config_line(line, cfg) sub-flow (called for each non-EOF line):**

12. Assertion: `line != nullptr`.
13. Skip leading whitespace (pointer `p` advances over spaces and tabs).
14. If `*p` is `#`, `;`, `\0`, `\n`, or `\r`: return silently (blank or comment line).
15. Declare `char key[64]` and `char val[64]` on stack; zero-initialise both with `memset`.
16. `int n = sscanf(p, "%63[^ \t=]%*[ \t=]%63s", key, val)`:
    - `%63[^ \t=]` — captures the key (up to 63 chars; stops at space, tab, or `=`).
    - `%*[ \t=]` — discards the separator.
    - `%63s` — captures the first non-whitespace value token.
17. If `n != 2`: log WARNING_LO "Skipping malformed config line: <first 60 chars>"; return.
18. Postcondition assertion: `key[0] != '\0'`.
19. Call `apply_kv(key, val, cfg)`.

**apply_kv(key, val, cfg) sub-flow (one recognised key at a time):**

20. Assertions: `key != nullptr`; `val != nullptr`.
21. `strcmp(key, ...)` switch-equivalent if/else chain:
    - `"enabled"` → `sscanf(val, "%u", &v)`; `cfg.enabled = (v != 0)`.
    - `"fixed_latency_ms"` → `sscanf(val, "%u", &v)`; `cfg.fixed_latency_ms = v`.
    - `"jitter_mean_ms"` → `sscanf(val, "%u", &v)`; `cfg.jitter_mean_ms = v`.
    - `"jitter_variance_ms"` → `sscanf(val, "%u", &v)`; `cfg.jitter_variance_ms = v`.
    - `"loss_probability"` → `sscanf(val, "%lf", &v)`; clamp: `if v < 0.0: v = 0.0`; `if v > 1.0: v = 1.0`; `cfg.loss_probability = v`.
    - `"duplication_probability"` → same clamp logic as `loss_probability`.
    - `"reorder_enabled"` → `sscanf(val, "%u", &v)`; `cfg.reorder_enabled = (v != 0)`.
    - `"reorder_window_size"` → `sscanf(val, "%u", &v)`; clamp to `IMPAIR_DELAY_BUF_SIZE` (32) if `v > IMPAIR_DELAY_BUF_SIZE` (log WARNING_LO on clamp); `cfg.reorder_window_size = v`.
    - `"partition_enabled"` → `sscanf(val, "%u", &v)`; `cfg.partition_enabled = (v != 0)`.
    - `"partition_duration_ms"` → `sscanf(val, "%u", &v)`; `cfg.partition_duration_ms = v`.
    - `"partition_gap_ms"` → `sscanf(val, "%u", &v)`; `cfg.partition_gap_ms = v`.
    - `"prng_seed"` → `sscanf(val, "%llu", &ull)`; `cfg.prng_seed = static_cast<uint64_t>(ull)`.
    - Any other key → log WARNING_LO "Unknown config key ignored: <key>".
22. Each `sscanf` call's return value is checked; if it does not equal 1, the field retains its default value.

---

## 4. Call Tree (Primary Success Path)

```
impairment_config_load()
 ├── impairment_config_default()          [always — ensures safe initial state]
 ├── fopen()                              [open file]
 ├── [loop ≤ 64 iterations]
 │    └── fgets()                         [read one line]
 │         └── parse_config_line()
 │              └── [skip if blank/comment]
 │              └── sscanf(key=value format)
 │              └── apply_kv()
 │                   └── strcmp(key)      [identify field]
 │                   └── sscanf(val)      [parse typed value]
 │                   └── [clamp if probability field]
 │                   └── cfg.<field> = v
 └── fclose()
```

---

## 5. Key Components Involved

- **`impairment_config_load()`** — public entry point; orchestrates open → loop → close.
- **`impairment_config_default()`** (in `ImpairmentConfig.hpp`) — inline function; zeroes all fields to safe defaults; always called first so missing keys are not undefined.
- **`parse_config_line()`** — file-local static helper; strips whitespace and comments, extracts key/value with `sscanf`, delegates to `apply_kv()`. One call per `fgets()` result.
- **`apply_kv()`** — file-local static helper; maps a string key to the corresponding `ImpairmentConfig` field; clamps probabilities to [0.0, 1.0] and `reorder_window_size` to `IMPAIR_DELAY_BUF_SIZE`.
- **`ImpairmentConfig`** (in `src/core/ImpairmentConfig.hpp`) — plain POD struct; no dynamic allocation; fields include `enabled`, `fixed_latency_ms`, `jitter_mean_ms`, `jitter_variance_ms`, `loss_probability`, `duplication_probability`, `reorder_enabled`, `reorder_window_size`, `partition_enabled`, `partition_duration_ms`, `partition_gap_ms`, `prng_seed`.
- **`Logger::log()`** — emits INFO on load start and completion; WARNING_LO for unknown keys, malformed lines, file overflow, `fclose` failure; WARNING_LO on `fopen` failure.
- **`MAX_CONFIG_LINES` = 64** — loop bound; defined in `ImpairmentConfigLoader.hpp`.
- **`MAX_CONFIG_LINE_LEN` = 128** — per-line buffer size; defined file-locally in `.cpp`.

---

## 6. Branching Logic / Decision Points

| Condition | True path | False path |
|-----------|-----------|------------|
| `fopen()` returns nullptr | Log WARNING_LO; return `ERR_IO` | Proceed to read loop |
| `fgets()` returns nullptr (EOF/error) | `eof_reached = true`; break | Call `parse_config_line(line, cfg)` |
| Loop counter reaches `MAX_CONFIG_LINES` (64) without EOF | Exit loop; log WARNING_LO (file truncated) | Continue reading |
| Line starts with `#`, `;`, `\0`, `\n`, or `\r` (after whitespace skip) | Return silently (blank/comment) | Proceed to sscanf |
| `sscanf(key=value)` returns != 2 | Log WARNING_LO; skip line | Call `apply_kv()` |
| Key is a known field | Apply value to cfg field | Log WARNING_LO; skip |
| `sscanf(val)` for the field returns != 1 | Field retains default (silent) | Update cfg field |
| `loss_probability < 0.0` | Clamp to 0.0 | Allow as-is |
| `loss_probability > 1.0` | Clamp to 1.0 | Allow as-is |
| `duplication_probability < 0.0` | Clamp to 0.0 | Allow as-is |
| `duplication_probability > 1.0` | Clamp to 1.0 | Allow as-is |
| `reorder_window_size > IMPAIR_DELAY_BUF_SIZE` | Clamp to 32; log WARNING_LO | Allow as-is |
| `fclose()` returns != 0 | Log WARNING_LO | — |
| `!eof_reached` after loop | Log WARNING_LO (file overflow) | — |

---

## 7. Concurrency / Threading Behavior

`impairment_config_load()` is a free function with no shared state. All state is local: the `FILE*` pointer, the `line[]` stack buffer, and the `cfg` output reference. The function is reentrant as long as callers pass distinct `cfg` objects.

If multiple threads call `impairment_config_load()` concurrently with the same file path, they each open an independent `FILE*` handle; POSIX guarantees that `fopen`/`fgets`/`fclose` on separate handles are independent.

No `std::atomic`, mutex, or synchronisation primitive is used.

---

## 8. Memory and Ownership Semantics

**Stack allocations in `impairment_config_load()`:**
- `char line[MAX_CONFIG_LINE_LEN]` (128 bytes) — line read buffer; reused on each iteration.
- `bool eof_reached` (1 byte).
- `FILE* fp` (pointer).

**Stack allocations in `parse_config_line()`:**
- `char key[64]`, `char val[64]` — key/value extraction buffers; zero-initialised with `memset` before each `sscanf`.
- `const char* p` — pointer into `line`; used for whitespace skip.

**Stack allocations in `apply_kv()`:**
- `uint32_t v` or `double v` or `unsigned long long ull` — temporaries for `sscanf` output.

**No heap allocations whatsoever** — Power of 10 Rule 3 is fully satisfied. No `new`, `malloc`, or STL containers.

**Ownership:**
- `cfg` is passed by reference; `impairment_config_load()` populates it in-place. The caller retains ownership.
- The `FILE*` is opened in `impairment_config_load()` and closed before the function returns (even on the no-EOF path). There is no resource leak path.
- In the `fopen()` failure path, `fp` is nullptr; `fclose()` is never called, so there is no double-free.

**`IMPAIR_DELAY_BUF_SIZE` = 32 (used to clamp `reorder_window_size`). `MAX_CONFIG_LINES` = 64. `MAX_CONFIG_LINE_LEN` = 128.**

---

## 9. Error Handling Flow

| Error event | `cfg` state | Return value |
|-------------|-------------|--------------|
| `path == nullptr` | NEVER_COMPILED_OUT_ASSERT fires | N/A (asserts) |
| `fopen()` fails | Contains defaults from `impairment_config_default()` | `ERR_IO` |
| `fgets()` returns nullptr on line 1 (empty file) | Contains defaults | `OK` (empty file is valid) |
| `sscanf` parse fails on a line | That line's field retains default | `OK` (parsing continues) |
| Unknown key encountered | Field not updated; WARNING_LO logged | `OK` (parsing continues) |
| `fclose()` returns non-zero | `cfg` already fully populated | `OK` |
| File has > 64 lines | Lines 65+ silently skipped; first 64 parsed | `OK` with WARNING_LO |

No error in the parsing loop causes early termination or affects unrelated fields. Each field is updated independently.

---

## 10. External Interactions

| Call | Purpose | Library |
|------|---------|---------|
| `fopen(path, "r")` | Open config file for reading | C standard library (POSIX file I/O) |
| `fgets(line, 128, fp)` | Read one line (up to 127 chars + NUL) | C standard library |
| `fclose(fp)` | Close file; flush OS buffer | C standard library |
| `sscanf(p, "%63[^ \t=]%*[ \t=]%63s", key, val)` | Extract key and value token | C standard library |
| `sscanf(val, "%u", &v)` | Parse unsigned integer value | C standard library |
| `sscanf(val, "%lf", &v)` | Parse double-precision float | C standard library |
| `sscanf(val, "%llu", &ull)` | Parse unsigned 64-bit int (prng_seed) | C standard library |
| `strcmp(key, ...)` | Identify recognised key | C standard library |
| `memset(key/val, 0, ...)` | Zero-initialise key/val buffers | C standard library |
| `Logger::log()` | Emit structured log entries | project Logger |

No network calls. No dynamic allocation. No threads.

---

## 11. State Changes / Side Effects

| What changes | When | Description |
|---|---|---|
| `cfg` (caller's struct) | `impairment_config_default()` | All fields set to safe defaults |
| `cfg.<field>` | `apply_kv()`, per matched key | Individual field updated from file |
| No `DtlsUdpBackend` / `TlsTcpBackend` state | — | This function is standalone; no transport state is touched |

Side effects:
- Log output at INFO (start + completion), WARNING_LO (errors, unknowns, overflow).
- The file at `path` is opened and closed. File access time is updated.

---

## 12. Sequence Diagram

```
User                     impairment_config_load()    C stdio / OS
  |                               |                       |
  |--- impairment_config_load() ->|                       |
  |                               |-- impairment_config_default(cfg)
  |                               |-- fopen(path,"r") --->|
  |                               |<-- fp (non-null) -----|
  |                               |-- [INFO log]          |
  |                               |                       |
  |                               | [loop i=0..63]        |
  |                               |-- fgets(line,128,fp)->|
  |                               |<-- "enabled = 1\n" ---|
  |                               | parse_config_line()   |
  |                               |   sscanf -> key="enabled", val="1"
  |                               |   apply_kv(): cfg.enabled = true
  |                               |-- fgets() ----------->|
  |                               |<-- "loss_probability = 0.05\n"
  |                               | parse_config_line()   |
  |                               |   apply_kv(): loss = 0.05 (in range)
  |                               |-- fgets() ----------->|
  |                               |<-- nullptr (EOF) -----|
  |                               | eof_reached = true; break
  |                               |-- fclose(fp) -------->|
  |                               |-- [ASSERT probabilities in range]
  |                               |-- [INFO log: summary] |
  |<--- Result::OK ---------------|                       |
```

---

## 13. Initialization vs Runtime Flow

**This function has no initialization vs runtime distinction.** It is a pure I/O utility called on-demand whenever the user wants to load a config file. It does not maintain any persistent state across calls.

**Typical usage pattern:**
1. Before calling `DtlsUdpBackend::init()` or `LocalSimHarness::init()`, the user calls `impairment_config_load()` to obtain a populated `ImpairmentConfig`.
2. The returned `cfg` is embedded in a `ChannelConfig` which is embedded in a `TransportConfig`.
3. `init()` calls `impairment_config_default()` internally as well; if the user has already loaded a config, they overwrite the channel config with the loaded values before calling `init()`.

---

## 14. Known Risks / Observations

- **`MAX_CONFIG_LINES = 64` truncation is silent by default:** The WARNING_LO log is emitted but `fopen` does not fail. A large config file (over 64 lines) may have critical settings on line 65 or later silently ignored. There is no mechanism to detect which settings were skipped.
- **`sscanf` locale sensitivity for floating-point:** `%lf` in `sscanf` respects the C locale decimal separator. On systems where the locale uses a comma as the decimal separator, `"0.05"` would fail to parse. This could cause loss/duplication probability to silently retain 0.0. The code does not call `setlocale(LC_NUMERIC, "C")` before parsing. [ASSUMPTION] The process runs with the default C/POSIX locale.
- **No validation of `fixed_latency_ms`, `partition_duration_ms`, etc.:** Integer fields are parsed with `%u` but are not range-clamped. A value of `UINT32_MAX` for `fixed_latency_ms` would be accepted; downstream code in `ImpairmentEngine` must handle extreme values safely.
- **`apply_kv()` has high cyclomatic complexity:** The if/else chain for 13 recognised keys has CC = 14, above the Power of 10 Rule 4 ceiling of 10. This is a known deviation; the function is pure data dispatch with no control flow interdependencies. A refactoring with a lookup table would require function pointers (Rule 9 issue) or STL (not permitted).
- **`fclose()` failure is non-fatal:** If `fclose()` returns non-zero (e.g., a write flush error on NFS), the function still returns `OK`. The parsed data is unaffected.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The C locale's decimal separator is `.` (period). If the locale uses `,`, `%lf` parsing of probabilities will fail silently and fields will retain their default values of 0.0.
- [ASSUMPTION] Lines in the config file are terminated by `\n` or `\r\n`. `fgets` reads until `\n` or EOF; CRLF line endings would leave a `\r` in the value token, which `sscanf("%63s")` would include in `val`. This could cause a probability parse failure for lines like `"loss_probability = 0.05\r"`. No explicit CRLF stripping is performed.
- [ASSUMPTION] The file at `path` is a regular file (not a named pipe or device). `fgets` on a blocking FIFO could block indefinitely.
- [ASSUMPTION] `IMPAIR_DELAY_BUF_SIZE` (32) is the correct upper bound for `reorder_window_size`. This constant is defined in `src/core/Types.hpp` and must match the buffer size in `ImpairmentEngine`.
- [ASSUMPTION] `sscanf` returning 0 or 1 for a field that requires 2 tokens (key + value) is treated as a malformed line. A line with a key but no value (e.g., `"enabled"` with no `=`) would be caught by `n != 2`.
