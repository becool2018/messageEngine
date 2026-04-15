# UC_27 — Configuration defaults applied and logged at init time

**HL Group:** HL-16 — System Initialization
**Actor:** User
**Requirement traceability:** REQ-4.2.1, REQ-4.2.2, REQ-7.1.2

---

## 1. Use Case Overview

- **Trigger:** User calls `transport_config_default(cfg)` and/or `channel_config_default(cfg)` before customizing and passing the config to `TcpBackend::init()` or `DeliveryEngine::init()`. Files: `src/core/ChannelConfig.hpp`.
- **Goal:** Pre-populate a `TransportConfig` or `ChannelConfig` with safe, well-known defaults so that the caller only needs to override fields that differ from the defaults.
- **Success outcome:** All fields in the config struct are set to safe defaults. The transport/engine initializes successfully with these defaults plus any user overrides.
- **Error outcomes:** None — these are simple in-place initialization functions with no failure modes.

---

## 2. Entry Points

```cpp
// src/core/ChannelConfig.hpp
void transport_config_default(TransportConfig& cfg);
void channel_config_default(ChannelConfig& cfg);
```

Also referenced in UC_47.

---

## 3. End-to-End Control Flow

1. User calls `transport_config_default(cfg)`:
   a. `memset(&cfg, 0, sizeof(cfg))` — zero all bytes.
   b. `cfg.kind = TransportKind::TCP` — default to TCP transport.
   c. `cfg.is_server = false`.
   d. `cfg.send_timeout_ms = 1000`.
   e. `cfg.recv_timeout_ms = 1000`.
   f. `cfg.connect_timeout_ms = 5000`.
   g. `cfg.local_node_id = NODE_ID_INVALID`.
   h. For each of `MAX_CHANNELS` channels: `channel_config_default(cfg.channels[i])`.
2. User calls `channel_config_default(ch)`:
   a. `memset(&ch, 0, sizeof(ch))` — zero all bytes.
   b. `ch.priority = 0`.
   c. `ch.reliability_class = ReliabilityClass::BEST_EFFORT`.
   d. `ch.max_retries = MAX_RETRY_COUNT` (5).
   e. `ch.retry_backoff_ms = 100`.
   f. `ch.recv_timeout_ms = 5000`.
3. User overrides specific fields as needed (e.g., `cfg.is_server = true`; `cfg.local_node_id = 1`).
4. `TcpBackend::init(cfg)` or `DeliveryEngine::init(transport, cfg.channels[0], node_id)` is called.
5. During init, `LOG_INFO(...)` logs the key configuration values.

---

## 4. Call Tree

```
transport_config_default(cfg)               [ChannelConfig.hpp]
 └── channel_config_default(cfg.channels[i])  [×MAX_CHANNELS]

[User customizes cfg]

TcpBackend::init(cfg)                       [TcpBackend.cpp]
 └── LOG_INFO("TcpBackend", "init: ip=%s port=%u ...")
```

---

## 5. Key Components Involved

- **`transport_config_default()`** — Zero-initializes and sets safe defaults for all `TransportConfig` fields, including nested `ChannelConfig` array.
- **`channel_config_default()`** — Zero-initializes and sets safe defaults for a single `ChannelConfig`.
- **Logger** — Used in `init()` functions to record the applied configuration for observability.

---

## 6. Branching Logic / Decision Points

No branching in the default functions themselves — they are unconditional assignments. Branching occurs during `init()` based on `is_server`, `kind`, etc.

---

## 7. Concurrency / Threading Behavior

- These are simple initialization functions called once at startup. Not thread-safe if called on a shared config simultaneously, but that would be a usage error.

---

## 8. Memory & Ownership Semantics

- `cfg` — caller-provided struct; written in-place. No heap allocation.
- `memset` ensures all padding bytes are zero.

---

## 9. Error Handling Flow

- No error states. All defaults are statically determined safe values.

---

## 10. External Interactions

- None during the default-setting phase.
- `LOG_INFO(...)` during the subsequent `init()` call writes to `stderr`.

---

## 11. State Changes / Side Effects

- `cfg` (caller-provided) — all fields set to defaults. Caller is responsible for customization after this call.

---

## 12. Sequence Diagram

```
User
  -> transport_config_default(cfg)
       -> channel_config_default(cfg.channels[i]) [×8]
  [User overrides: cfg.is_server=true; cfg.local_node_id=1; etc.]
  -> TcpBackend::init(cfg)
       -> LOG_INFO("init: ip=%s port=%u is_server=%d ...")
```

---

## 13. Initialization vs Runtime Flow

**Before `init()`:** `transport_config_default()` must be called before passing `cfg` to any `init()` function. Not calling it leaves fields in an undefined state (uninitialized stack or heap memory).

**After `init()`:** Config values are copied into the backend's `m_cfg` member. Changing the original `cfg` struct after `init()` has no effect.

---

## 14. Known Risks / Observations

- **`NODE_ID_INVALID = 0` as default:** `local_node_id` defaults to 0 (invalid). The user MUST set a valid non-zero `local_node_id` before passing to `DeliveryEngine::init()`.
- **Logging of sensitive fields:** `LOG_INFO()` in `init()` may log IP addresses and port numbers to stderr. REQ-7.1.4 restricts payload logging but configuration values are considered metadata.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `transport_config_default()` and `channel_config_default()` are inline functions defined in `ChannelConfig.hpp`, not in a separate `.cpp` file. This is inferred from the header-only pattern used for simple POD initialization helpers.
