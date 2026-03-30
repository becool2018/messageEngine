# UC_27 — Config defaults and overrides

**HL Group:** HL-16 — User initializes the system
**Actor:** User
**Requirement traceability:** REQ-4.2.1, REQ-4.2.2

---

## 1. Use Case Overview

### What triggers this flow

The application entry point (`main()` in `Client.cpp` or `Server.cpp`) declares a `TransportConfig` on the stack and calls `transport_config_default()` to fill it with safe defaults, then applies per-role overrides before passing the struct to `TcpBackend::init()` and `DeliveryEngine::init()`.

### Expected outcome (single goal)

The `TransportConfig` (including its embedded `ChannelConfig channels[0]`) is fully populated with well-defined values, all overrides are applied, and the struct is ready to be consumed by the transport and delivery engine without further modification.

---

## 2. Entry Points

**Primary factory functions:**
- `inline void transport_config_default(TransportConfig& cfg)` — `src/core/ChannelConfig.hpp`, line 68. Called at `Client.cpp:241`, `Server.cpp:208`.
- `inline void channel_config_default(ChannelConfig& cfg, uint8_t id)` — `src/core/ChannelConfig.hpp`, line 53. Called from within `transport_config_default()` at line 96, and again explicitly from `Client.cpp:255` and `Server.cpp:221`.

**Override sites:**
- `Client.cpp`, lines 243–259: post-default overrides for the client role.
- `Server.cpp`, lines 210–225: post-default overrides for the server role.

**Consumers of the final config:**
- `TcpBackend::init(cfg)` — `Client.cpp:265`, `Server.cpp:231`.
- `DeliveryEngine::init(&transport, cfg.channels[0], node_id)` — `Client.cpp:277`, `Server.cpp:243`.

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase A — transport_config_default(cfg)**

1. Caller declares `TransportConfig cfg` on the stack in `main()`. Fields are indeterminate until `transport_config_default()` is called.
2. `transport_config_default(cfg)` is called (`ChannelConfig.hpp:68`).
3. Total zero-fill: `(void)__builtin_memset(&cfg, 0, sizeof(TransportConfig))`. Sets every byte of the entire struct to 0, including both 48-byte IP arrays, all integer fields, all `MAX_CHANNELS` `ChannelConfig` slots, and the `bool is_server`.
4. Scalar field assignments:
   - `cfg.kind               = TransportKind::TCP`
   - `cfg.bind_port          = 9000U`
   - `cfg.peer_port          = 9000U`
   - `cfg.connect_timeout_ms = 5000U`
   - `cfg.num_channels       = 1U`
   - `cfg.local_node_id      = 1U`
   - `cfg.is_server          = false`
5. IP string copy loop (`ChannelConfig.hpp:85-94`):
   - `const char* lo = "127.0.0.1"` ; `static const int LO_MAX = 9` (= `sizeof("127.0.0.1") - 1`)
   - `while (lo[i] != '\0' && i < LO_MAX)`: copies 9 characters into `cfg.bind_ip[i]` and `cfg.peer_ip[i]`.
   - `cfg.bind_ip[9] = '\0'`; `cfg.peer_ip[9] = '\0'`.
   - Loop bound is the source literal length (LO_MAX=9), not the destination capacity (47). Power of 10 Rule 2.
   - `cfg.bind_ip[10..47]` and `cfg.peer_ip[10..47]` remain 0 from the `__builtin_memset`.
6. `channel_config_default(cfg.channels[0], 0U)` called (`ChannelConfig.hpp:53`):
   - `cfg.channel_id         = 0U`
   - `cfg.priority           = 0U` (= `static_cast<uint8_t>(id)` = 0)
   - `cfg.reliability        = ReliabilityClass::BEST_EFFORT`
   - `cfg.ordering           = OrderingMode::UNORDERED`
   - `cfg.max_queue_depth    = MSG_RING_CAPACITY` (= 64U)
   - `cfg.send_timeout_ms    = 1000U`
   - `cfg.recv_timeout_ms    = 1000U`
   - `cfg.max_retries        = MAX_RETRY_COUNT` (= 5U)
   - `cfg.retry_backoff_ms   = 100U`
   - `cfg.impairments_enabled = false`
   - `channels[1..MAX_CHANNELS-1]` remain all-zeros from `__builtin_memset`. They are not valid `ChannelConfig` objects.
7. `transport_config_default()` returns. `cfg` now holds a complete, well-defined default configuration.

**Phase B — Client.cpp overrides (Client.cpp:240–259)**

8. Post-default overrides applied unconditionally in `main()`:
   - `cfg.kind = TransportKind::TCP` (redundant; same as default)
   - `cfg.is_server = false` (redundant)
   - `cfg.peer_port = peer_port` — from `parse_peer_port(argc, argv)`: `strtol(argv[2])` with end\_ptr validation; returns `DEFAULT_PEER_PORT = 9000` if `argc < 3` or value out of `[1..65535]`.
   - `cfg.connect_timeout_ms = 5000U` (redundant)
   - `cfg.local_node_id = LOCAL_CLIENT_NODE_ID` = 2U. **Key change: 1U → 2U.**
   - `cfg.num_channels = 1U` (redundant)
   - `(void)strncpy(cfg.peer_ip, peer_ip, sizeof(cfg.peer_ip) - 1U)` + `cfg.peer_ip[47] = '\0'` — from `parse_peer_ip(argc, argv)`: `argv[1]` or `DEFAULT_PEER_IP`. `cfg.bind_ip` is NOT modified.
   - `channel_config_default(cfg.channels[0], 0U)` called again — re-applies all channel defaults before channel-specific overrides.
   - `cfg.channels[0].reliability = ReliabilityClass::RELIABLE_RETRY`. **Key change: BEST\_EFFORT → RELIABLE\_RETRY.**
   - `cfg.channels[0].ordering = OrderingMode::UNORDERED` (redundant)
   - `cfg.channels[0].max_retries = 3U`. **Key change: 5U → 3U.**
   - `cfg.channels[0].recv_timeout_ms = static_cast<uint32_t>(RECV_TIMEOUT_MS)` = 100U. **Key change: 1000U → 100U.**

**Phase C — Server.cpp overrides (Server.cpp:207–225)**

9. Post-default overrides applied unconditionally in `main()`:
   - `cfg.kind = TransportKind::TCP` (redundant)
   - `cfg.is_server = true`. **Key change: false → true.** Tells `TcpBackend` to listen/bind/accept.
   - `cfg.bind_port = bind_port` — from `parse_server_port(argc, argv)`: `strtol(argv[1])` or `DEFAULT_BIND_PORT = 9000`.
   - `cfg.local_node_id = LOCAL_SERVER_NODE_ID` = 1U (redundant; default is also 1U)
   - `cfg.num_channels = 1U` (redundant)
   - `(void)strncpy(cfg.bind_ip, DEFAULT_BIND_IP, sizeof(cfg.bind_ip) - 1U)` + `cfg.bind_ip[47] = '\0'`. `DEFAULT_BIND_IP = "0.0.0.0"`. **Key change: "127.0.0.1" → "0.0.0.0".** `cfg.peer_ip` is NOT modified.
   - `channel_config_default(cfg.channels[0], 0U)` called again.
   - `cfg.channels[0].reliability = ReliabilityClass::RELIABLE_RETRY`. **Key change: BEST\_EFFORT → RELIABLE\_RETRY.**
   - `cfg.channels[0].ordering = OrderingMode::ORDERED`. **Key change: UNORDERED → ORDERED. Differs from client.**
   - `cfg.channels[0].max_retries = 3U`. **Key change: 5U → 3U.**
   - `cfg.channels[0].recv_timeout_ms = 100U`. **Key change: 1000U → 100U.**

**Phase D — Config consumption**

10. `TcpBackend transport; transport.init(cfg)` — `Client.cpp:264-265` / `Server.cpp:230-231`. Result checked; `FATAL` logged and process exits on failure.
11. `DeliveryEngine engine; engine.init(&transport, cfg.channels[0], node_id)` — `Client.cpp:277` / `Server.cpp:243`. Receives a pointer to `TcpBackend` and a copy of `cfg.channels[0]`. See UC\_28 for the `init()` internals.

---

## 4. Call Tree (Hierarchical)

```
[Client.cpp main()]
Client::main()                                          [Client.cpp]
  ├── parse_peer_ip(argc, argv)
  ├── parse_peer_port(argc, argv)
  ├── transport_config_default(cfg)                     [ChannelConfig.hpp:68]
  │   ├── __builtin_memset(&cfg, 0, sizeof cfg)
  │   ├── cfg.kind=TCP; ports; timeout; num_channels=1; node_id=1; is_server=false
  │   ├── [while i<LO_MAX=9] cfg.bind_ip[i]=lo[i]; cfg.peer_ip[i]=lo[i]
  │   └── channel_config_default(cfg.channels[0], 0U)  [ChannelConfig.hpp:53]
  │       └── [10 field assignments: id=0, priority=0, BEST_EFFORT, UNORDERED,
  │             max_queue_depth=64, send_timeout=1000, recv_timeout=1000,
  │             max_retries=5, retry_backoff=100, impairments_enabled=false]
  ├── [overrides] cfg.local_node_id=2; strncpy(peer_ip, ...)
  ├── channel_config_default(cfg.channels[0], 0U)       [re-init]
  ├── [channel overrides]
  │   ├── reliability=RELIABLE_RETRY  [KEY]
  │   ├── max_retries=3               [KEY]
  │   └── recv_timeout_ms=100         [KEY]
  ├── TcpBackend::init(cfg)
  └── DeliveryEngine::init(&transport, channels[0], 2)

[Server.cpp main()]
Server::main()                                          [Server.cpp]
  ├── parse_server_port(argc, argv)
  ├── transport_config_default(cfg)                     [identical tree]
  ├── [overrides] cfg.is_server=true; cfg.bind_port=...; strncpy(bind_ip,"0.0.0.0")
  ├── channel_config_default(cfg.channels[0], 0U)       [re-init]
  ├── [channel overrides]
  │   ├── reliability=RELIABLE_RETRY  [KEY]
  │   ├── ordering=ORDERED            [KEY — differs from client]
  │   ├── max_retries=3               [KEY]
  │   └── recv_timeout_ms=100         [KEY]
  ├── TcpBackend::init(cfg)
  └── DeliveryEngine::init(&transport, channels[0], 1)
```

---

## 5. Key Components Involved

**`TransportConfig` — `src/core/ChannelConfig.hpp:39`**
Plain struct. Contains `kind`, 48-byte `bind_ip`/`peer_ip`, `bind_port`, `peer_port`, `connect_timeout_ms`, `num_channels`, `ChannelConfig channels[MAX_CHANNELS]`, `local_node_id`, `is_server`. Stack-allocated in `main()`.

**`ChannelConfig` — `src/core/ChannelConfig.hpp:23`**
Plain struct. 10 fields: `channel_id`, `priority`, `reliability`, `ordering`, `max_queue_depth`, `send_timeout_ms`, `recv_timeout_ms`, `max_retries`, `retry_backoff_ms`, `impairments_enabled`. Embedded as array of `MAX_CHANNELS` in `TransportConfig`.

**`transport_config_default()` — `src/core/ChannelConfig.hpp:68`**
Inline. Zero-fills the entire struct, then sets 7 scalar fields, the IP strings, and calls `channel_config_default(channels[0])`.

**`channel_config_default()` — `src/core/ChannelConfig.hpp:53`**
Inline. Sets all 10 `ChannelConfig` fields. `priority = id` (lower id = higher priority). Called both inside `transport_config_default()` and again explicitly by each application before channel-specific overrides.

**`TcpBackend` — `src/platform/TcpBackend.hpp`**
Consumer of the final `TransportConfig` via `init(cfg)`. Uses `is_server` to choose listen vs. connect mode.

**`DeliveryEngine` — `src/core/DeliveryEngine.hpp`**
Consumer of `cfg.channels[0]` and `local_node_id` via `init()`. Stores `cfg.channels[0]` by value (`m_cfg = cfg` at `DeliveryEngine.cpp:17`). Independent of the caller's `cfg` after `init()` returns.

---

## 6. Branching Logic / Decision Points

**Branch 1 — IP string copy loop** (`ChannelConfig.hpp:88`)
- Condition: `lo[i] != '\0' && i < LO_MAX` where `LO_MAX = 9`.
- Runs exactly 9 times; terminates on null terminator. Power of 10 Rule 2: bound by source length.

**Branch 2 — `parse_peer_port()` argc check** (`Client.cpp`)
- Condition: `argc >= 3`
- True: `strtol(argv[2])` with `end_ptr` validation.
- False: return `DEFAULT_PEER_PORT = 9000`.

**Branch 3 — port range check** (`Client.cpp`)
- Condition: `port_val > 0 && port_val <= 65535`
- True: return `static_cast<uint16_t>(port_val)`.
- False: return `DEFAULT_PEER_PORT = 9000` (silent fallback).

**Branch 4 — `parse_server_port()` argc check** (`Server.cpp`)
- Same pattern as Branch 2, for `argv[1]` and `DEFAULT_BIND_PORT`.

**Branch 5 — TcpBackend init result** (`Client.cpp:266`, `Server.cpp:232`)
- Condition: `!result_ok(res)`
- True: `Logger::log(FATAL, ...)` + `return 1` (process exits).
- False: proceed to `DeliveryEngine::init()`.

All config assignment statements in both application files are unconditional.

---

## 7. Concurrency / Threading Behavior

Configuration construction and override occur entirely within the single `main()` thread of each process, before `TcpBackend::init()` and `DeliveryEngine::init()` are called. No concurrent access to `cfg` during this phase.

After `init()` calls complete, `cfg` is not accessed again. `TcpBackend` and `DeliveryEngine` both copy the fields they need during their `init()` calls and thereafter operate from their own internal copies.

No synchronization primitives are needed for configuration construction.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

**`TransportConfig cfg`:**
Stack-allocated local variable in `main()`. Passed by reference to `transport_config_default()`, `channel_config_default()`, `TcpBackend::init()`, and `DeliveryEngine::init()`. No heap allocation. Lives for the entire duration of `main()`.

**`__builtin_memset`:**
GCC/Clang built-in (non-standard; see RISK-1). Zero-fills the entire struct. `(void)` cast applied to the return value.

**`ChannelConfig channels[MAX_CHANNELS]`:**
Inline array within `TransportConfig`. After `__builtin_memset`, `channels[1..MAX_CHANNELS-1]` are all-zeros and are not valid `ChannelConfig` objects. Only `channels[0]` is explicitly initialized.

**`strncpy` calls** (`Client.cpp:251`, `Server.cpp:217`):
Used for IP string copies with explicit null-termination at `[sizeof - 1]`. `(void)` cast on return value. Safe bounded copy.

**`DeliveryEngine::init()`:**
Receives `cfg.channels[0]` by value (a plain struct copy). After `init()` returns, `DeliveryEngine` is independent of the original `cfg` stack variable. No pointers to `cfg` escape.

---

## 9. Error Handling Flow

`transport_config_default()` and `channel_config_default()` are `void` functions and cannot fail.

Port argument validation (`parse_peer_port` / `parse_server_port`): uses `strtol` with `end_ptr` check. If argument is non-numeric or out of `[1..65535]`, silently returns the default port with no warning log.

`TcpBackend::init()` failure: `Client.cpp:266-270` / `Server.cpp:232-236`: `if (!result_ok(res))` → `Logger::log(FATAL, ...)` → `return 1`. Process exits if transport initialization fails.

`DeliveryEngine::init()` is `void` and has no error return path. All error detection is via `NEVER_COMPILED_OUT_ASSERT`.

---

## 10. External Interactions

**`__builtin_memset` (`ChannelConfig.hpp:70`):**
GCC/Clang built-in. Zero-fills the `TransportConfig` struct. Potentially expanded inline.

**`strncpy` (`Client.cpp:251`, `Server.cpp:217`):**
C standard library. Bounded IP string copy with explicit null-termination.

**`strtol` (`Client.cpp`, `Server.cpp`):**
C standard library. Robust decimal integer parsing with `end_ptr` validation.

No socket calls, no file I/O, and no logging during the config construction and override phase. Logging begins at the `TcpBackend::init()` result check.

---

## 11. State Changes / Side Effects

**`transport_config_default(cfg)` writes to the full `TransportConfig`:**
- `cfg.kind = TransportKind::TCP`; `cfg.bind_ip = "127.0.0.1"`; `cfg.bind_port = 9000U`; `cfg.peer_ip = "127.0.0.1"`; `cfg.peer_port = 9000U`; `cfg.connect_timeout_ms = 5000U`; `cfg.num_channels = 1U`; `cfg.local_node_id = 1U`; `cfg.is_server = false`
- `cfg.channels[0]`: all 10 fields per `channel_config_default`
- `cfg.channels[1..MAX_CHANNELS-1]`: all-zeros

**Client.cpp final state after all overrides:**

| Field | Default | Final |
|---|---|---|
| `local_node_id` | 1U | **2U** |
| `peer_ip` | "127.0.0.1" | argv[1] or "127.0.0.1" |
| `peer_port` | 9000U | argv[2] or 9000U |
| `channels[0].reliability` | BEST\_EFFORT | **RELIABLE\_RETRY** |
| `channels[0].max_retries` | 5U | **3U** |
| `channels[0].recv_timeout_ms` | 1000U | **100U** |
| `channels[0].ordering` | UNORDERED | UNORDERED (redundant) |

**Server.cpp final state after all overrides:**

| Field | Default | Final |
|---|---|---|
| `is_server` | false | **true** |
| `bind_ip` | "127.0.0.1" | **"0.0.0.0"** |
| `bind_port` | 9000U | argv[1] or 9000U |
| `channels[0].reliability` | BEST\_EFFORT | **RELIABLE\_RETRY** |
| `channels[0].ordering` | UNORDERED | **ORDERED** |
| `channels[0].max_retries` | 5U | **3U** |
| `channels[0].recv_timeout_ms` | 1000U | **100U** |

---

## 12. Sequence Diagram using mermaid

```mermaid
sequenceDiagram
    participant main as main() [Client or Server]
    participant tcd as transport_config_default()
    participant ccd as channel_config_default()
    participant TcpBE as TcpBackend::init()
    participant DE as DeliveryEngine::init()

    main->>tcd: transport_config_default(cfg)
    tcd->>tcd: __builtin_memset(cfg, 0)
    tcd->>tcd: cfg.kind=TCP; ports=9000; timeout=5000; num_channels=1; node_id=1; is_server=false
    tcd->>tcd: while i<9: cfg.bind_ip[i]=lo[i]; cfg.peer_ip[i]=lo[i]
    tcd->>ccd: channel_config_default(cfg.channels[0], 0U)
    ccd->>ccd: set all 10 ChannelConfig fields to defaults
    ccd-->>tcd: done
    tcd-->>main: done

    note over main: Apply post-default overrides
    main->>main: cfg.local_node_id=2 [client] or cfg.is_server=true [server]
    main->>main: strncpy(cfg.peer_ip or cfg.bind_ip, ...)
    main->>ccd: channel_config_default(cfg.channels[0], 0U) [re-init]
    ccd-->>main: done
    main->>main: cfg.channels[0].reliability=RELIABLE_RETRY
    main->>main: cfg.channels[0].ordering=UNORDERED [client] or ORDERED [server]
    main->>main: cfg.channels[0].max_retries=3; recv_timeout_ms=100

    main->>TcpBE: transport.init(cfg)
    TcpBE-->>main: Result::OK or FATAL exit

    main->>DE: engine.init(&transport, cfg.channels[0], node_id)
    DE-->>main: done (void)
```

---

## 13. Initialization vs Runtime Flow

### Startup / initialization (one-time, before main loop)

1. `transport_config_default()` zero-fills `cfg` and sets safe defaults.
2. Per-role overrides applied inline in `main()` (unconditional assignments).
3. `channel_config_default()` called a second time to reset `channels[0]` before channel-specific overrides.
4. `TcpBackend::init(cfg)` copies what it needs from `cfg` into `TcpBackend` state.
5. `DeliveryEngine::init(&transport, cfg.channels[0], node_id)` copies `channels[0]` into `m_cfg`.

### Steady-state runtime

`cfg` has no runtime role. No config fields are read or modified during the send/receive loops. `TcpBackend` and `DeliveryEngine` operate from their own internal copies of configuration fields.

---

## 14. Known Risks / Observations

**RISK-1 — `__builtin_memset` is a non-standard GCC/Clang extension:**
`ChannelConfig.hpp:70` uses `__builtin_memset` rather than the standard C `memset` from `<cstring>`. MSVC and other conforming C++17 compilers do not provide this built-in. Using standard `memset` would be portable and MISRA-compliant. No documented justification in the code.

**RISK-2 — `channels[1..MAX_CHANNELS-1]` left as all-zeros:**
Only `channels[0]` is initialized by `transport_config_default()`. If `num_channels` is later increased to > 1 without calling `channel_config_default()` for each additional channel, `TcpBackend` or `DeliveryEngine` would consume zero-filled `ChannelConfig` objects: `channel_id=0` (collision with channel 0), `priority=0`, `max_queue_depth=0`, all timeouts=0.

**RISK-3 — `channel_config_default()` called twice per application:**
Each application calls `channel_config_default(channels[0], 0U)` inside `transport_config_default()` and then again explicitly before the channel-specific overrides. The second call re-applies all defaults, resetting any values the first call set. If `transport_config_default()` were extended to set non-default channel values, the second explicit call would silently reset them.

**RISK-4 — Silent fallback on invalid port argument:**
`parse_peer_port()` and `parse_server_port()` return the default port 9000 silently with no `WARNING` or `INFO` log if the argument is non-numeric or out of range. A user providing an invalid port observes the program connecting on port 9000 without notice.

**RISK-5 — `OrderingMode` asymmetry between client and server:**
Client `channels[0]`: `UNORDERED`. Server `channels[0]`: `ORDERED`. If the ordered receive path requires sequence numbers or resequencing state that the unordered sender does not provide, delivery behavior may differ from expectations.

**RISK-6 — `local_node_id` default (1U) collides with `SERVER_NODE_ID` (1U):**
The default `local_node_id` is 1U and the server retains it. Any third node initialized with the default and no override would also claim `node_id = 1`, colliding with the server. Node ID uniqueness is not enforced anywhere in the configuration layer.

---

## 15. Unknowns / Assumptions

`[CONFIRMED]` `TcpBackend` is the transport consumer. `Client.cpp:264` declares `TcpBackend transport`; `Client.cpp:265` calls `transport.init(cfg)`.

`[CONFIRMED]` `DeliveryEngine` is the channel config consumer. `Client.cpp:276-277`: `DeliveryEngine engine; engine.init(&transport, cfg.channels[0], LOCAL_CLIENT_NODE_ID)`.

`[CONFIRMED]` `transport_config_default()` IP copy loop uses `LO_MAX = 9` (source string length), not `i < 47`. This was corrected from an earlier `i < 47` bound.

`[CONFIRMED]` Client and server IP copies use `strncpy` with explicit null-termination, not a manual while-loop.

`[CONFIRMED]` `MSG_RING_CAPACITY = 64U`, `MAX_RETRY_COUNT = 5U` from `Types.hpp`.

`[CONFIRMED]` `DeliveryEngine::init()` stores `cfg.channels[0]` by value (`m_cfg = cfg` at `DeliveryEngine.cpp:17`). After `init()` returns, `DeliveryEngine` does not hold a pointer to the caller's `cfg`.

`[ASSUMPTION]` `TcpBackend::init()` copies all fields it needs from `TransportConfig` into its own internal state. `TcpBackend.cpp` was not read; inferred from the fact that `cfg` is a stack local in `main()`.

`[ASSUMPTION]` `TcpBackend` uses `cfg.is_server` to distinguish listen/accept mode (server) from connect mode (client).
