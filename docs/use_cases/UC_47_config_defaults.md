# UC_47 — transport_config_default() and channel_config_default(): User fills TransportConfig and ChannelConfig with safe defaults before customising

**HL Group:** HL-16 — System Initialization
**Actor:** User
**Requirement traceability:** REQ-4.2.1, REQ-4.2.2, REQ-6.3.3, REQ-4.1.1

---

## 1. Use Case Overview

- **Trigger:** User calls `transport_config_default()` or `channel_config_default()` before customizing fields and passing the result to `TcpBackend::init()` / `UdpBackend::init()` / `DtlsUdpBackend::init()`. Files: `src/core/ChannelConfig.hpp`, `src/platform/TcpBackend.hpp`.
- **Goal:** Provide a fully populated `TransportConfig` or `ChannelConfig` with safe, known-good default values so that the caller only needs to override the fields that differ from the standard configuration.
- **Success outcome:** Returns a `TransportConfig` or `ChannelConfig` struct with all fields set to documented safe defaults. The caller modifies selected fields and passes the result to `init()`.
- **Error outcomes:** None — these are value-returning factory functions with no failure modes.

---

## 2. Entry Points

```cpp
// src/core/ChannelConfig.hpp or src/platform/TcpBackend.hpp
TransportConfig transport_config_default();
ChannelConfig   channel_config_default();
```

---

## 3. End-to-End Control Flow

**`transport_config_default()`:**
1. Declare a local `TransportConfig cfg`.
2. Set `cfg.is_server = false`.
3. Set `cfg.peer_ip[0] = '\0'` (empty peer address).
4. Set `cfg.peer_port = 0`.
5. Set `cfg.bind_ip = "127.0.0.1"` (loopback by default; caller sets to `"0.0.0.0"` or a specific interface for network-accessible deployments).
6. Set `cfg.bind_port = 0`.
7. Set `cfg.connect_timeout_ms = 5000` (5-second connect timeout).
8. Set `cfg.recv_timeout_ms = 1000` (1-second receive timeout).
9. Set `cfg.tls = tls_config_default()` — zero-initialized TLS config with `tls_enabled = false`.
10. Return `cfg`.

**`channel_config_default()`:**
1. Declare a local `ChannelConfig cfg`.
2. Set `cfg.priority = 0` (lowest priority).
3. Set `cfg.reliability = ReliabilityClass::BEST_EFFORT`.
4. Set `cfg.ordered = false`.
5. Set `cfg.impairment = impairment_config_default()` — all impairments disabled.
6. Return `cfg`.

---

## 4. Call Tree

```
transport_config_default()                         [ChannelConfig.hpp / TcpBackend.hpp]
 └── tls_config_default()                          [TlsConfig initialized with tls_enabled=false]

channel_config_default()                           [ChannelConfig.hpp]
 └── impairment_config_default()                   [all impairments disabled, probabilities=0.0]
```

---

## 5. Key Components Involved

- **`transport_config_default()`** — produces a `TransportConfig` with safe socket timeouts and TLS disabled. The caller sets `is_server`, `peer_ip`, `peer_port`, and/or `bind_port` before calling `init()`.
- **`channel_config_default()`** — produces a `ChannelConfig` with BEST_EFFORT reliability and no impairments. The caller enables specific impairments or changes reliability as needed.
- **`impairment_config_default()`** — all probabilities 0.0, all durations 0, all flags false. ImpairmentEngine will pass through all messages unchanged.

---

## 6. Branching Logic / Decision Points

No branching. These are simple struct-populating factory functions.

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| (none) | N/A | N/A |

---

## 7. Concurrency / Threading Behavior

- These are pure value-returning functions. No shared state is accessed.
- Safe to call from any thread at any time.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- Return by value; the struct is stack-allocated in the caller's frame after the return.
- No heap allocation. Power of 10 Rule 3 compliant.
- `TransportConfig` contains `char peer_ip[INET_ADDRSTRLEN]`, `char bind_ip[INET_ADDRSTRLEN]`, `TlsConfig tls`, and scalar fields.
- `ChannelConfig` contains `ImpairmentConfig impairment` and scalar fields.

---

## 9. Error Handling Flow

- No error states. These functions always succeed.

---

## 10. External Interactions

- None — these functions operate entirely in process memory with no OS calls.

---

## 11. State Changes / Side Effects

None. These are pure factory functions; no global or static state is modified.

---

## 12. Sequence Diagram

```
User -> transport_config_default()
  <- TransportConfig { is_server=false, connect_timeout_ms=5000, tls.tls_enabled=false, ... }

User: cfg.is_server = true; cfg.bind_port = 9000;

User -> TcpBackend::init(cfg)
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** None.

**Runtime:** Called once per transport instance during setup. The returned struct is customized and passed to `init()`. The struct is then stored as `m_cfg` inside the backend.

---

## 14. Known Risks / Observations

- **Default is client mode:** `transport_config_default()` returns `is_server = false`. Server deployments must explicitly set `cfg.is_server = true` and `cfg.bind_port`.
- **TLS disabled by default:** `tls.tls_enabled = false`. Callers requiring TLS must explicitly set `cfg.tls.tls_enabled = true` and populate cert/key/CA paths.
- **Zero bind_port:** A `bind_port` of 0 triggers OS-assigned ephemeral port; this may be intentional for client mode but will produce a non-deterministic port for server mode.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Default `connect_timeout_ms = 5000` and `recv_timeout_ms = 1000`; exact values are inferred from usage in tests and the application layer.
- `[ASSUMPTION]` `tls_config_default()` zero-initializes all TLS cert path strings and sets `tls_enabled = false`, `verify_peer = false`.
- `[ASSUMPTION]` `channel_config_default()` sets `reliability = BEST_EFFORT` and all `ImpairmentConfig` probabilities to 0.0.
