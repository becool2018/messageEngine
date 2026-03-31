# UC_27 — Configuration defaults applied and logged at init time
**HL Group:** HL-16 — System Initialization
**Actor:** User
**Requirement traceability:** REQ-4.2.1, REQ-4.2.2

---

## 1. Use Case Overview

### What triggers this flow

The application entry point (`main()` in `Client.cpp` or `Server.cpp`) declares a
`TransportConfig` on the stack and calls `transport_config_default()` to fill it with safe
defaults, then applies per-role overrides before passing the struct to `TcpBackend::init()` and
`DeliveryEngine::init()`.

### Expected outcome (single goal)

The `TransportConfig` (including its embedded `ChannelConfig channels[0]`) is fully populated
with well-defined values, all overrides are applied, and the struct is ready to be consumed by
the transport and delivery engine without further modification.

---

## 2. Entry Points

Primary factory functions:

- `inline void transport_config_default(TransportConfig& cfg)` — src/core/ChannelConfig.hpp,
  line 68. Called from `Client.cpp:241`, `Server.cpp:208`.
- `inline void channel_config_default(ChannelConfig& cfg, uint8_t id)` — src/core/ChannelConfig.hpp,
  line 56. Called from within `transport_config_default()` at line 99, and again explicitly from
  `Client.cpp:255` and `Server.cpp:221`.
- `inline void impairment_config_default(ImpairmentConfig& cfg)` — src/core/ImpairmentConfig.hpp,
  line 84. Called from within `channel_config_default()` at line 67.

Override sites:
- `Client.cpp`, lines 243–259: post-default overrides for the client role.
- `Server.cpp`, lines 210–225: post-default overrides for the server role.

Consumers of the final config:
- `TcpBackend::init(cfg)` — `Client.cpp:265`, `Server.cpp:231`.
- `DeliveryEngine::init(&transport, cfg.channels[0], node_id)` — `Client.cpp:277`, `Server.cpp:243`.

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase A — transport_config_default(cfg)**

1. Caller declares `TransportConfig cfg` on the stack in `main()`. Fields are indeterminate
   until `transport_config_default()` is called.

2. `transport_config_default(cfg)` is called (ChannelConfig.hpp:71).

3. Total zero-fill:
   ```
   (void)__builtin_memset(&cfg, 0, sizeof(TransportConfig))
   ```
   Sets every byte of the entire struct to 0, including both 48-byte IP arrays, all integer
   fields, all `MAX_CHANNELS` `ChannelConfig` slots, the embedded `TlsConfig`, and the
   `bool is_server`.

4. Scalar field assignments (ChannelConfig.hpp:74–80):
   - `cfg.kind               = TransportKind::TCP`
   - `cfg.bind_port          = 9000U`
   - `cfg.peer_port          = 9000U`
   - `cfg.connect_timeout_ms = 5000U`
   - `cfg.num_channels       = 1U`
   - `cfg.local_node_id      = 1U`
   - `cfg.is_server          = false`

5. IP string copy loop (ChannelConfig.hpp:88–97):
   - `const char* lo = "127.0.0.1"`.
   - `static const int LO_MAX = static_cast<int>(sizeof("127.0.0.1")) - 1` = 9.
   - `while (lo[i] != '\0' && i < LO_MAX)`: copies exactly 9 characters into
     `cfg.bind_ip[i]` and `cfg.peer_ip[i]`.
   - `cfg.bind_ip[9] = '\0'`; `cfg.peer_ip[9] = '\0'`.
   - Bound is the source literal length (`LO_MAX=9`), not the destination capacity (47).
     Power of 10 Rule 2.
   - `cfg.bind_ip[10..47]` and `cfg.peer_ip[10..47]` remain 0 from the `__builtin_memset`.

6. `channel_config_default(cfg.channels[0], 0U)` called (ChannelConfig.hpp:56):
   - `cfg.channels[0].channel_id         = 0U`
   - `cfg.channels[0].priority           = 0U` (= `static_cast<uint8_t>(id)`)
   - `cfg.channels[0].reliability        = ReliabilityClass::BEST_EFFORT`
   - `cfg.channels[0].ordering           = OrderingMode::UNORDERED`
   - `cfg.channels[0].max_queue_depth    = MSG_RING_CAPACITY` (= 64U)
   - `cfg.channels[0].send_timeout_ms    = 1000U`
   - `cfg.channels[0].recv_timeout_ms    = 1000U`
   - `cfg.channels[0].max_retries        = MAX_RETRY_COUNT` (= 5U)
   - `cfg.channels[0].retry_backoff_ms   = 100U`
   - `impairment_config_default(cfg.channels[0].impairment)` called inline:
     - `enabled = false`; all probabilities = 0.0; all latencies = 0U;
       `reorder_enabled = false`; `partition_enabled = false`; `prng_seed = 42ULL`.
   - `channels[1..MAX_CHANNELS-1]` remain all-zeros from `__builtin_memset`.

7. `tls_config_default(cfg.tls)` called (ChannelConfig.hpp:100).
   Sets `tls_enabled = false`; clears all TLS cert/key/CA path strings. TLS is off by default.

8. `transport_config_default()` returns. `cfg` now holds a complete, well-defined default
   configuration.

**Phase B — Client.cpp overrides**

9. Post-default overrides applied unconditionally in `main()`:
   - `cfg.kind = TransportKind::TCP` (redundant)
   - `cfg.is_server = false` (redundant)
   - `cfg.peer_port = peer_port` — from `parse_peer_port(argc, argv)`.
   - `cfg.connect_timeout_ms = 5000U` (redundant)
   - `cfg.local_node_id = LOCAL_CLIENT_NODE_ID` = 2U. **Key change: 1U → 2U.**
   - `cfg.num_channels = 1U` (redundant)
   - `(void)strncpy(cfg.peer_ip, peer_ip, sizeof(cfg.peer_ip) - 1U)` + `cfg.peer_ip[47] = '\0'`.
     `cfg.bind_ip` is NOT modified.
   - `channel_config_default(cfg.channels[0], 0U)` called again — re-applies all channel
     defaults before channel-specific overrides.
   - `cfg.channels[0].reliability = ReliabilityClass::RELIABLE_RETRY`. **Key change.**
   - `cfg.channels[0].ordering = OrderingMode::UNORDERED` (redundant)
   - `cfg.channels[0].max_retries = 3U`. **Key change: 5U → 3U.**
   - `cfg.channels[0].recv_timeout_ms = 100U`. **Key change: 1000U → 100U.**

**Phase C — Server.cpp overrides**

10. Post-default overrides applied unconditionally in `main()`:
    - `cfg.is_server = true`. **Key change: false → true.**
    - `cfg.bind_port = bind_port` — from `parse_server_port(argc, argv)`.
    - `cfg.local_node_id = LOCAL_SERVER_NODE_ID` = 1U (redundant)
    - `cfg.num_channels = 1U` (redundant)
    - `(void)strncpy(cfg.bind_ip, DEFAULT_BIND_IP, sizeof(cfg.bind_ip) - 1U)` + null-terminate.
      `DEFAULT_BIND_IP = "0.0.0.0"`. **Key change: "127.0.0.1" → "0.0.0.0".**
      `cfg.peer_ip` is NOT modified.
    - `channel_config_default(cfg.channels[0], 0U)` called again.
    - `cfg.channels[0].reliability = ReliabilityClass::RELIABLE_RETRY`. **Key change.**
    - `cfg.channels[0].ordering = OrderingMode::ORDERED`. **Key change: differs from client.**
    - `cfg.channels[0].max_retries = 3U`. **Key change: 5U → 3U.**
    - `cfg.channels[0].recv_timeout_ms = 100U`. **Key change: 1000U → 100U.**

**Phase D — Config consumption**

11. `TcpBackend transport; transport.init(cfg)` — result checked; FATAL logged and process
    exits on failure.

12. `DeliveryEngine engine; engine.init(&transport, cfg.channels[0], node_id)` — receives
    a pointer to `TcpBackend` and a copy of `cfg.channels[0]`. See UC_28 for internals.

---

## 4. Call Tree (Hierarchical)

```
[Client.cpp main()]
  ├── parse_peer_ip(argc, argv)
  ├── parse_peer_port(argc, argv)
  ├── transport_config_default(cfg)                     [ChannelConfig.hpp:71]
  │    ├── __builtin_memset(&cfg, 0, sizeof(TransportConfig))
  │    ├── cfg.kind=TCP; ports=9000; timeout=5000; num_channels=1; node_id=1; is_server=false
  │    ├── [while i<LO_MAX=9] cfg.bind_ip[i]=lo[i]; cfg.peer_ip[i]=lo[i]
  │    ├── channel_config_default(cfg.channels[0], 0U)  [ChannelConfig.hpp:56]
  │    │    ├── [9 field assignments]
  │    │    └── impairment_config_default(cfg.channels[0].impairment)
  │    │         └── [all impairments disabled; prng_seed=42ULL]
  │    └── tls_config_default(cfg.tls)
  │         └── [tls_enabled=false; paths cleared]
  ├── [overrides] cfg.local_node_id=2; strncpy(peer_ip, ...)
  ├── channel_config_default(cfg.channels[0], 0U)       [re-init before channel overrides]
  ├── [channel overrides]
  │    ├── reliability=RELIABLE_RETRY                   [KEY]
  │    ├── max_retries=3                                [KEY]
  │    └── recv_timeout_ms=100                          [KEY]
  ├── TcpBackend::init(cfg)
  └── DeliveryEngine::init(&transport, channels[0], 2)

[Server.cpp main()]
  ├── parse_server_port(argc, argv)
  ├── transport_config_default(cfg)                     [identical tree as above]
  ├── [overrides] cfg.is_server=true; strncpy(bind_ip, "0.0.0.0", ...)
  ├── channel_config_default(cfg.channels[0], 0U)       [re-init]
  ├── [channel overrides]
  │    ├── reliability=RELIABLE_RETRY                   [KEY]
  │    ├── ordering=ORDERED                             [KEY — differs from client]
  │    ├── max_retries=3                                [KEY]
  │    └── recv_timeout_ms=100                          [KEY]
  ├── TcpBackend::init(cfg)
  └── DeliveryEngine::init(&transport, channels[0], 1)
```

---

## 5. Key Components Involved

| Component | File(s) | Role |
|-----------|---------|------|
| `TransportConfig` | src/core/ChannelConfig.hpp:41 | Top-level config struct. Stack-allocated in `main()`. Contains `kind`, 48-byte IP arrays, ports, timeouts, `ChannelConfig channels[MAX_CHANNELS]`, `local_node_id`, `is_server`, `TlsConfig`. |
| `ChannelConfig` | src/core/ChannelConfig.hpp:25 | Per-channel config embedded as array of `MAX_CHANNELS` inside `TransportConfig`. |
| `ImpairmentConfig` | src/core/ImpairmentConfig.hpp:37 | Embedded inside `ChannelConfig`. All impairments disabled by default. |
| `transport_config_default()` | src/core/ChannelConfig.hpp:71 | Inline. Zero-fills entire struct, then sets 7 scalar fields, IP strings (9-char bounded loop), and calls `channel_config_default` and `tls_config_default`. |
| `channel_config_default()` | src/core/ChannelConfig.hpp:56 | Inline. Sets all 9 `ChannelConfig` fields. Calls `impairment_config_default`. |
| `impairment_config_default()` | src/core/ImpairmentConfig.hpp:84 | Inline. All impairments off; `prng_seed=42ULL`. |
| `TcpBackend` | src/platform/TcpBackend.hpp | Consumer of `TransportConfig` via `init(cfg)`. Uses `is_server` to choose listen vs connect mode. |
| `DeliveryEngine` | src/core/DeliveryEngine.hpp | Consumer of `cfg.channels[0]` and `local_node_id`. Stores the channel config by value in `m_cfg`. Independent of caller's `cfg` after `init()` returns. |

---

## 6. Branching Logic / Decision Points

| Branch | Condition | True path | False path |
|--------|-----------|-----------|------------|
| IP string copy loop | `lo[i] != '\0' && i < LO_MAX` | Copy character; advance `i` | Terminate loop (loop runs exactly 9 times) |
| `parse_peer_port()` argc check | `argc >= 3` | `strtol(argv[2])` with end_ptr validation | Return `DEFAULT_PEER_PORT = 9000` |
| Port range check | `port_val > 0 && port_val <= 65535` | Return `static_cast<uint16_t>(port_val)` | Return `DEFAULT_PEER_PORT = 9000` (silent fallback) |
| `parse_server_port()` argc check | Same pattern as above | `strtol(argv[1])` with end_ptr | Return `DEFAULT_BIND_PORT = 9000` |
| TcpBackend init result | `!result_ok(res)` | `Logger::log(FATAL)` + `return 1` (process exits) | Proceed to `DeliveryEngine::init()` |

All config assignment statements in the override sections are unconditional.

---

## 7. Concurrency / Threading Behavior

Configuration construction and all override assignments occur entirely within the single
`main()` thread, before `TcpBackend::init()` and `DeliveryEngine::init()` are called. No
concurrent access to `cfg` during this phase.

After `init()` calls complete, `cfg` is not accessed again. `TcpBackend` and `DeliveryEngine`
both copy the fields they need during their `init()` calls and operate thereafter from their
own internal copies.

No synchronization primitives are needed for configuration construction.

---

## 8. Memory & Ownership Semantics

| Object | Storage | Notes |
|--------|---------|-------|
| `TransportConfig cfg` | Stack local in `main()` | Zero-filled by `__builtin_memset`, then field-assigned. Lives for the entire duration of `main()`. |
| `cfg.channels[0]` (inline) | Inside `cfg` on the stack | Default-initialized by `channel_config_default`. The second call to `channel_config_default` re-applies defaults before overrides. |
| `cfg.channels[1..MAX_CHANNELS-1]` (inline) | Inside `cfg` | All-zeros from `__builtin_memset`. Not valid `ChannelConfig` objects. |
| `cfg.tls` (inline) | Inside `cfg` | Zero-filled by `__builtin_memset`; then `tls_config_default()` sets `tls_enabled=false`. |
| `TcpBackend transport` | Stack local in `main()` | Receives a copy of `cfg` via `init(cfg)`. |
| `DeliveryEngine engine` | Stack local in `main()` | Receives `cfg.channels[0]` by value; copies into `m_cfg`. No pointer to `cfg` retained. |

No heap allocation. `strncpy` is the only C runtime call during the override phase; it writes
into the fixed 48-byte IP arrays.

---

## 9. Error Handling Flow

`transport_config_default()`, `channel_config_default()`, and `impairment_config_default()` are
all `void` functions and cannot fail.

Port argument validation: uses `strtol` with `end_ptr` check. If argument is non-numeric or
out of `[1..65535]`, silently returns the default port with no warning log. See Risk 4.

`TcpBackend::init()` failure: `if (!result_ok(res))` → `Logger::log(FATAL, ...)` → `return 1`.
Process exits if transport initialization fails.

`DeliveryEngine::init()` is `void` and has no error return path. All error detection is via
`NEVER_COMPILED_OUT_ASSERT`. See UC_28.

---

## 10. External Interactions

`__builtin_memset` (ChannelConfig.hpp:73): GCC/Clang built-in. Zero-fills the `TransportConfig`
struct. Non-standard; see Risk 1.

`strncpy` (Client.cpp, Server.cpp): C standard library. Bounded IP string copy with explicit
null-termination at `[sizeof - 1]`. Return value void-cast.

`strtol` (Client.cpp, Server.cpp): C standard library. Decimal integer parsing with `end_ptr`
validation.

No socket calls, no file I/O, and no logging during config construction. Logging begins only at
the `TcpBackend::init()` result check.

---

## 11. State Changes / Side Effects

`transport_config_default(cfg)` writes to the full `TransportConfig` on the caller's stack:
- `kind = TCP`; `bind_ip = "127.0.0.1"`; `bind_port = 9000U`; `peer_ip = "127.0.0.1"`;
  `peer_port = 9000U`; `connect_timeout_ms = 5000U`; `num_channels = 1U`;
  `local_node_id = 1U`; `is_server = false`.
- `channels[0]`: 9 fields per `channel_config_default` + all impairments disabled.
- `tls`: `tls_enabled = false`.
- `channels[1..MAX_CHANNELS-1]`: all-zeros (not valid configs).

Client.cpp final state after all overrides:

| Field | Default | Final |
|-------|---------|-------|
| `local_node_id` | 1U | **2U** |
| `peer_ip` | "127.0.0.1" | argv[1] or "127.0.0.1" |
| `peer_port` | 9000U | argv[2] or 9000U |
| `channels[0].reliability` | BEST_EFFORT | **RELIABLE_RETRY** |
| `channels[0].max_retries` | 5U | **3U** |
| `channels[0].recv_timeout_ms` | 1000U | **100U** |
| `channels[0].ordering` | UNORDERED | UNORDERED (redundant) |

Server.cpp final state after all overrides:

| Field | Default | Final |
|-------|---------|-------|
| `is_server` | false | **true** |
| `bind_ip` | "127.0.0.1" | **"0.0.0.0"** |
| `bind_port` | 9000U | argv[1] or 9000U |
| `channels[0].reliability` | BEST_EFFORT | **RELIABLE_RETRY** |
| `channels[0].ordering` | UNORDERED | **ORDERED** |
| `channels[0].max_retries` | 5U | **3U** |
| `channels[0].recv_timeout_ms` | 1000U | **100U** |

---

## 12. Sequence Diagram

```
main() [Client or Server]
  |
  |-- transport_config_default(cfg) -----------> transport_config_default()
  |                                                  |
  |                                                  |-- __builtin_memset(&cfg, 0)
  |                                                  |-- cfg.kind=TCP; bind_port=9000; peer_port=9000
  |                                                  |-- cfg.connect_timeout_ms=5000; num_channels=1
  |                                                  |-- cfg.local_node_id=1; cfg.is_server=false
  |                                                  |-- [i=0..8] cfg.bind_ip[i]=lo[i]; cfg.peer_ip[i]=lo[i]
  |                                                  |-- cfg.bind_ip[9]='\0'; cfg.peer_ip[9]='\0'
  |                                                  |
  |                                                  |-- channel_config_default(cfg.channels[0], 0U) ->
  |                                                  |       channel_config_default()
  |                                                  |           |-- id=0; priority=0; BEST_EFFORT; UNORDERED
  |                                                  |           |-- max_queue_depth=64; send_timeout=1000
  |                                                  |           |-- recv_timeout=1000; max_retries=5
  |                                                  |           |-- retry_backoff_ms=100
  |                                                  |           |-- impairment_config_default()
  |                                                  |                 enabled=false; prng_seed=42ULL
  |                                                  |
  |                                                  |-- tls_config_default(cfg.tls)
  |                                                  |       tls_enabled=false; paths cleared
  |                                                  |
  |<-- (void) <---------------------------------------
  |
  |-- [overrides] cfg.local_node_id=2 (client) or cfg.is_server=true (server)
  |-- [overrides] strncpy(cfg.peer_ip, ...) or strncpy(cfg.bind_ip, "0.0.0.0", ...)
  |
  |-- channel_config_default(cfg.channels[0], 0U)    [re-init before channel overrides]
  |
  |-- [channel overrides]
  |       cfg.channels[0].reliability = RELIABLE_RETRY
  |       cfg.channels[0].ordering = UNORDERED (client) or ORDERED (server)
  |       cfg.channels[0].max_retries = 3
  |       cfg.channels[0].recv_timeout_ms = 100
  |
  |-- transport.init(cfg) -----------------------> TcpBackend::init(cfg)
  |                                                  copies cfg into TcpBackend state
  |<-- Result::OK or FATAL exit <-----------------
  |
  |-- engine.init(&transport, cfg.channels[0], id) -> DeliveryEngine::init()
  |                                                  copies channels[0] into m_cfg
  |<-- (void) <---------------------------------------
```

---

## 13. Initialization vs Runtime Flow

### Startup / initialization (one-time, before main loop)

1. `transport_config_default()` zero-fills `cfg` and sets safe defaults. One call to
   `channel_config_default()` and one to `tls_config_default()` inside.
2. Per-role overrides applied inline in `main()` (unconditional assignments).
3. `channel_config_default()` called a second time to reset `channels[0]` before
   channel-specific overrides.
4. `TcpBackend::init(cfg)` copies what it needs from `cfg` into `TcpBackend` state.
5. `DeliveryEngine::init(&transport, cfg.channels[0], node_id)` copies `channels[0]`
   into `m_cfg`.

### Steady-state runtime

`cfg` has no runtime role. No config fields are read or modified during the send/receive loops.
`TcpBackend` and `DeliveryEngine` operate from their own internal copies.

---

## 14. Known Risks / Observations

### Risk 1 — `__builtin_memset` is a non-standard extension
ChannelConfig.hpp:73 uses `__builtin_memset` rather than the standard C `memset` from
`<cstring>`. MSVC and other conforming C++17 compilers do not provide this built-in. Standard
`memset` would be portable and MISRA-compliant. No documented justification in the code.

### Risk 2 — `channels[1..MAX_CHANNELS-1]` left as all-zeros
Only `channels[0]` is initialized by `transport_config_default()`. If `num_channels` is later
increased to > 1 without calling `channel_config_default()` for each additional channel, the
consumers would see zero-filled `ChannelConfig` objects: `channel_id=0` (collision),
`priority=0`, `max_queue_depth=0`, all timeouts=0.

### Risk 3 — `channel_config_default()` called twice per application
Each application calls `channel_config_default(channels[0], 0U)` inside
`transport_config_default()` and again explicitly before channel-specific overrides. The second
call re-applies all defaults, resetting any values the first call established. If
`transport_config_default()` were extended to set non-default channel values, the second
explicit call would silently reset them.

### Risk 4 — Silent fallback on invalid port argument
`parse_peer_port()` and `parse_server_port()` return the default port 9000 silently with no
WARNING or INFO log if the argument is non-numeric or out of range. A user providing an
invalid port observes the program connecting on port 9000 without notice.

### Risk 5 — `OrderingMode` asymmetry between client and server
Client `channels[0]`: `UNORDERED`. Server `channels[0]`: `ORDERED`. If the ordered receive
path requires sequence numbers or resequencing state that the unordered sender does not
provide, delivery behavior may differ from expectations.

### Risk 6 — `local_node_id` default (1U) collides with `SERVER_NODE_ID` (1U)
The default `local_node_id` is 1U and the server retains it. Any third node initialized with
the default and no override would also claim `node_id = 1`, colliding with the server. Node ID
uniqueness is not enforced anywhere in the configuration layer.

---

## 15. Unknowns / Assumptions

**[CONFIRMED-1]** `TcpBackend` is the transport consumer. `Client.cpp:264` declares
`TcpBackend transport`; `Client.cpp:265` calls `transport.init(cfg)`.

**[CONFIRMED-2]** `DeliveryEngine` is the channel config consumer. `Client.cpp:276-277`:
`DeliveryEngine engine; engine.init(&transport, cfg.channels[0], LOCAL_CLIENT_NODE_ID)`.

**[CONFIRMED-3]** `transport_config_default()` IP copy loop uses `LO_MAX = 9` (source string
length), not `i < 47`. Corrected from an earlier `i < 47` bound (cppcheck
arrayIndexOutOfBoundsCond fix documented in ChannelConfig.hpp:85-87).

**[CONFIRMED-4]** Client and server IP copies use `strncpy` with explicit null-termination
at `[sizeof - 1]`.

**[CONFIRMED-5]** `MSG_RING_CAPACITY = 64U`, `MAX_RETRY_COUNT = 5U` from src/core/Types.hpp.

**[CONFIRMED-6]** `DeliveryEngine::init()` stores `cfg.channels[0]` by value (`m_cfg = cfg`
at DeliveryEngine.cpp:25). After `init()` returns, `DeliveryEngine` does not hold a pointer
to the caller's `cfg`.

**[CONFIRMED-7]** `impairment_config_default()` is called inside `channel_config_default()` at
ChannelConfig.hpp:67. The `ImpairmentConfig` is fully embedded in `ChannelConfig`; no separate
call is needed from the application.

**[ASSUMPTION-A]** `TcpBackend::init()` copies all fields it needs from `TransportConfig` into
its own internal state. Inferred from the fact that `cfg` is a stack local in `main()` that
goes out of scope after the initialization phase.

**[ASSUMPTION-B]** `TcpBackend` uses `cfg.is_server` to distinguish listen/accept mode (server)
from connect mode (client).
