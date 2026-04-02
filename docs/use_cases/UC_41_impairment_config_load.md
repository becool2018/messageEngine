# UC_41 — impairment_config_load(): read file, parse all recognised keys, clamp probabilities, return populated ImpairmentConfig

**HL Group:** HL-20 — Load Impairment Config from File
**Actor:** User
**Requirement traceability:** REQ-5.2.1, REQ-5.2.2, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** User calls `impairment_config_load(path, &out_cfg)` with a path to a key=value text file. File: `src/platform/ImpairmentConfig.cpp` (or `src/core/ImpairmentConfig.cpp`).
- **Goal:** Read the named file, parse all recognised key=value lines into the corresponding `ImpairmentConfig` fields, clamp any probability values to `[0.0, 1.0]`, and return a fully populated `ImpairmentConfig`.
- **Success outcome:** Returns `Result::OK`. `out_cfg` is populated with parsed values; unrecognised keys are silently skipped; unset keys retain default values.
- **Error outcomes:**
  - `Result::ERR_IO` — file cannot be opened at `path`.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentConfig.cpp
Result impairment_config_load(const char* path, ImpairmentConfig* out_cfg);
```

---

## 3. End-to-End Control Flow

1. **`impairment_config_load(path, out_cfg)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(path != nullptr)`.
3. `NEVER_COMPILED_OUT_ASSERT(out_cfg != nullptr)`.
4. `*out_cfg = impairment_config_default()` — initialize with safe defaults before parsing.
5. `::fopen(path, "r")` — open the file. If null: log `WARNING_LO`; return `ERR_IO`.
6. **Line-by-line parse loop** (bounded by file EOF — Rule 2 infrastructure deviation, bounded per-iteration work):
   a. `::fgets(line_buf, sizeof(line_buf), fp)` — read one line.
   b. Skip blank lines and comment lines (starting with `#`).
   c. Split on `=` delimiter; extract key and value strings.
   d. Match key against known field names (e.g., `"loss_probability"`, `"jitter_mean_ms"`, `"latency_ms"`, `"partition_duration_ms"`, `"partition_gap_ms"`, `"seed"`, `"duplication_probability"`, `"reorder_probability"`).
   e. Parse value with `strtod()` or `strtoul()` as appropriate.
   f. For probability fields: clamp to `[0.0, 1.0]` using `if (v < 0.0) v = 0.0; if (v > 1.0) v = 1.0;`.
   g. Assign to the corresponding `out_cfg` field.
7. `::fclose(fp)`.
8. Returns `Result::OK`.

---

## 4. Call Tree

```
impairment_config_load(path, out_cfg)              [ImpairmentConfig.cpp]
 ├── impairment_config_default()                   [returns zeroed/safe config]
 ├── ::fopen(path, "r")                            [POSIX C file I/O]
 ├── [parse loop]
 │    ├── ::fgets()                                [one line per iteration]
 │    ├── [key match against known fields]
 │    ├── ::strtod() / ::strtoul()                 [value parse]
 │    └── [clamp to [0.0, 1.0] for probabilities]
 └── ::fclose(fp)
```

---

## 5. Key Components Involved

- **`impairment_config_default()`** — initializes `out_cfg` to safe zero/disabled defaults before parsing, ensuring no field is left uninitialised if absent from the file.
- **Line parser** — key=value text parsing using `strtod()`/`strtoul()`; unrecognised keys skipped without error.
- **Probability clamp** — ensures `loss_probability`, `duplication_probability`, `reorder_probability` stay in `[0.0, 1.0]` regardless of what the file contains (REQ-5.2.2).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `::fopen()` returns null | Log WARNING_LO; return ERR_IO | Proceed with parsing |
| Line starts with `#` or is blank | Skip | Parse as key=value |
| Key is recognised | Parse and assign field | Skip line |
| Probability value < 0.0 | Clamp to 0.0 | Keep parsed value |
| Probability value > 1.0 | Clamp to 1.0 | Keep parsed value |

---

## 7. Concurrency / Threading Behavior

- `impairment_config_load()` is called during initialization; single-threaded.
- The returned `ImpairmentConfig` is a value type; the caller owns it after return.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `line_buf[256]` — stack-allocated line read buffer.
- `out_cfg` — caller-provided pointer to an `ImpairmentConfig` struct; written in place.
- No heap allocation. Power of 10 Rule 3 compliant.
- `fp` — `FILE*`; opened, used, and closed within the function; no resource leak on the success path. On error path, function returns before `fclose()` since file was never opened.

---

## 9. Error Handling Flow

- **File open failure:** `ERR_IO` returned; `*out_cfg` remains at default values (from `impairment_config_default()` called before open). Caller can use defaults or propagate the error.
- **Parse errors:** Malformed values silently produce `strtod()`'s default behavior (0.0 for unrecognized strings); no error is returned. Fields remain at their last assigned value.
- **Unrecognised keys:** Silently skipped; `out_cfg` field for that key is unchanged from its default.

---

## 10. External Interactions

- **POSIX C file I/O:** `::fopen()`, `::fgets()`, `::fclose()` — reads the config file line by line.
- No network I/O, socket operations, or signals.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `*out_cfg` | all fields | undefined | populated from file (or defaults) |

The function has no side effects on any global or static state. Only `*out_cfg` is modified.

---

## 12. Sequence Diagram

```
User -> impairment_config_load(path, &cfg)
  -> impairment_config_default()                   [zero-initialise cfg]
  -> ::fopen(path, "r")
  [parse loop: each line]
       -> ::fgets(line_buf, sizeof, fp)
       [skip comments and blank lines]
       [split on '=', match key]
       -> ::strtod() / ::strtoul()
       [clamp probabilities to [0.0, 1.0]]
       [assign to cfg field]
  -> ::fclose(fp)
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- Config file exists and is readable at `path`.
- `out_cfg` pointer is valid and points to an `ImpairmentConfig` the caller owns.

**Runtime:**
- This is an initialization-time function. It is called once before `ImpairmentEngine::init()` to produce the `ImpairmentConfig` used for the entire session.

---

## 14. Known Risks / Observations

- **Silent parse errors:** `strtod()` returns 0.0 for unrecognised strings; no warning is logged. A typo in a probability value will silently use 0.0 rather than the intended value.
- **Line length limit:** `line_buf` is fixed-size (inferred ~256 bytes). Lines longer than the buffer are silently truncated; this could corrupt the last field on a very long line.
- **No schema validation:** The parser does not validate that required keys are present. Missing keys produce defaults, not errors.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `impairment_config_load()` uses `::fgets()` and a fixed line buffer rather than C++ streams; this is consistent with the no-STL rule for production code.
- `[ASSUMPTION]` The recognized key names match the `ImpairmentConfig` struct field names; the exact names are inferred from `ImpairmentConfig.hpp` field definitions.
- `[ASSUMPTION]` The line buffer size is approximately 256 bytes; exact size inferred from typical config parsing patterns in the codebase.
