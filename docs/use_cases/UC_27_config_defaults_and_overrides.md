## 1. Use Case Overview

Use Case: UC-27 — TransportConfig and ChannelConfig are constructed with defaults and overridden per use.

The ChannelConfig.hpp header provides two inline factory functions: channel_config_default() and transport_config_default(). These establish safe, well-defined initial values for all configuration fields. Both Client.cpp and Server.cpp call transport_config_default() to obtain a base configuration, then selectively override specific fields to tailor behavior for their role (client vs. server, different ordering modes, different node IDs, different bind targets). This use case traces every field set by the default functions and every field overridden by each application file.

Scope: ChannelConfig.hpp lines 51-89; Client.cpp lines 188-212; Server.cpp lines 149-171.

---

## 2. Entry Points

Primary factory functions:
    inline void transport_config_default(TransportConfig& cfg)
        ChannelConfig.hpp line 66. Called by Client.cpp line 189, Server.cpp line 150.

    inline void channel_config_default(ChannelConfig& cfg, uint8_t id)
        ChannelConfig.hpp line 51.
        Called directly by transport_config_default() at line 88.
        Called again by Client.cpp line 207 and Server.cpp line 167 to reset
        channels[0] before applying per-app overrides.

Override sites:
    Client.cpp lines 191-211 — post-default overrides for client role.
    Server.cpp lines 152-171 — post-default overrides for server role.

---

## 3. End-to-End Control Flow (Step-by-Step)

--- 3A. transport_config_default() ---

Step 1 — Caller declares TransportConfig cfg on the stack (Client.cpp line 188;
    Server.cpp line 149). No initialization at declaration; struct fields are
    indeterminate.

Step 2 — transport_config_default(cfg) is called (Client.cpp line 189; Server.cpp line 150).

Step 3 — Total zero-fill (ChannelConfig.hpp line 68).
    __builtin_memset(&cfg, 0, sizeof(TransportConfig))
    Note: this uses __builtin_memset, not the standard memset.
    [OBSERVATION: use of __builtin_memset is a GCC/Clang extension and a MISRA
    deviation. Standard memset would be more portable and compliant. The (void)
    cast is applied.] This sets all bytes to 0, including all char arrays
    (bind_ip, peer_ip), all numeric fields, and all ChannelConfig slots.

Step 4 — Field assignments in transport_config_default() (lines 69-76):
    cfg.kind               = TransportKind::TCP        (enum value 0U)
    cfg.bind_port          = 9000U
    cfg.peer_port          = 9000U
    cfg.connect_timeout_ms = 5000U
    cfg.num_channels       = 1U
    cfg.local_node_id      = 1U
    cfg.is_server          = false

Step 5 — bind_ip and peer_ip set to "127.0.0.1" (lines 78-86).
    Bounded loop: while (lo[i] != '\0' && i < 47)
        cfg.bind_ip[i] = lo[i]
        cfg.peer_ip[i] = lo[i]
        ++i
    Null terminator written after loop at cfg.bind_ip[i] = '\0' and cfg.peer_ip[i] = '\0'.
    Both 48-byte char arrays are set to "127.0.0.1\0" with remaining bytes still 0
    from the memset.

Step 6 — channel_config_default(cfg.channels[0], 0U) called (line 88).
    [OBSERVATION: only channels[0] is initialized here. channels[1..7] remain
    all-zeros from memset. They are not valid ChannelConfig objects; accessing them
    without first calling channel_config_default() is a latent risk if num_channels
    is later increased without corresponding init calls.]

    Inside channel_config_default(cfg, id=0):
        cfg.channel_id          = 0U   (= id)
        cfg.priority            = 0U   (= static_cast<uint8_t>(id) = 0)
        cfg.reliability         = ReliabilityClass::BEST_EFFORT  (enum value 0U)
        cfg.ordering            = OrderingMode::UNORDERED         (enum value 1U)
        cfg.max_queue_depth     = MSG_RING_CAPACITY               (= 64U)
        cfg.send_timeout_ms     = 1000U
        cfg.recv_timeout_ms     = 1000U
        cfg.max_retries         = MAX_RETRY_COUNT                 (= 5U)
        cfg.retry_backoff_ms    = 100U
        cfg.impairments_enabled = false

Step 7 — transport_config_default() returns. cfg now holds a complete,
    valid default configuration.

--- 3B. Client.cpp Overrides (lines 191-211) ---

Step 8 — Client.cpp overrides (applied after transport_config_default returns):

    8a. cfg.kind            = TransportKind::TCP     (line 191)
        [REDUNDANT: same value already set by default. Explicit for clarity.]

    8b. cfg.is_server       = false                  (line 192)
        [REDUNDANT: same value already set by default. Explicit for clarity.]

    8c. cfg.peer_port       = peer_port              (line 193)
        peer_port comes from argv[2] or DEFAULT_PEER_PORT = 9000.
        If no argument: peer_port = 9000, same as default.
        If argument provided: peer_port = atoi(argv[2]), validated 1..65535.

    8d. cfg.connect_timeout_ms = 5000U               (line 194)
        [REDUNDANT: same value as default. Explicit for clarity.]

    8e. cfg.local_node_id   = LOCAL_CLIENT_NODE_ID   (line 195)
        LOCAL_CLIENT_NODE_ID = 2U (Client.cpp line 48).
        DEFAULT: local_node_id = 1U. This IS a real override (1 -> 2).

    8f. cfg.num_channels    = 1U                     (line 196)
        [REDUNDANT: same value as default.]

    8g. peer_ip copy loop (lines 199-204):
        Copies peer_ip argument (or "127.0.0.1") into cfg.peer_ip[].
        If peer_ip == DEFAULT_PEER_IP = "127.0.0.1": same as default.
        If argv[1] provided: cfg.peer_ip[] overwritten with new IP string.
        Note: cfg.bind_ip is NOT overridden in Client.cpp. It retains "127.0.0.1"
        from the default. [OBSERVATION: For a TCP client, bind_ip is not used
        for the outgoing connection target; peer_ip is what matters. The default
        bind_ip is irrelevant unless TcpBackend uses it for the client socket bind
        call, which is unusual.]

    8h. channel_config_default(cfg.channels[0], 0U) called again (line 207).
        This re-applies all ChannelConfig defaults to channels[0], RESETTING
        any fields that transport_config_default() had set. This is a full
        re-initialization of channels[0] before the per-channel overrides below.

    8i. cfg.channels[0].reliability = ReliabilityClass::RELIABLE_RETRY (line 208)
        Override: BEST_EFFORT (default) -> RELIABLE_RETRY (enum value 2U).
        Enables retry and deduplication semantics for channel 0.

    8j. cfg.channels[0].ordering    = OrderingMode::UNORDERED           (line 209)
        [REDUNDANT: UNORDERED is already the default. Explicit for clarity.]

    8k. cfg.channels[0].max_retries = 3U                                (line 210)
        Override: MAX_RETRY_COUNT (5U, default) -> 3U.
        Limits retries to 3 attempts per message on the client.

    8l. cfg.channels[0].recv_timeout_ms = RECV_TIMEOUT_MS               (line 211)
        RECV_TIMEOUT_MS = 100 (Client.cpp line 46). static_cast<uint32_t>(100).
        Override: 1000U (default) -> 100U. Shorter receive timeout for client.

--- 3C. Server.cpp Overrides (lines 152-171) ---

Step 9 — Server.cpp overrides (applied after transport_config_default returns):

    9a. cfg.kind            = TransportKind::TCP     (line 152)
        [REDUNDANT: same as default.]

    9b. cfg.is_server       = true                   (line 153)
        Override: false (default) -> true. Critical: tells TcpBackend to
        listen/bind/accept rather than connect.

    9c. cfg.bind_port       = bind_port              (line 154)
        bind_port comes from argv[1] or DEFAULT_BIND_PORT = 9000.
        If no argument: bind_port = 9000, same as default.
        If argument: bind_port = atoi(argv[1]), validated 1..65535.

    9d. cfg.local_node_id   = LOCAL_SERVER_NODE_ID   (line 155)
        LOCAL_SERVER_NODE_ID = 1U (Server.cpp line 44).
        DEFAULT: local_node_id = 1U. [REDUNDANT: same value as default. The
        server claims node ID 1, which matches the default.]

    9e. cfg.num_channels    = 1U                     (line 156)
        [REDUNDANT: same as default.]

    9f. bind_ip copy loop (lines 159-164):
        Copies DEFAULT_BIND_IP = "0.0.0.0" into cfg.bind_ip[].
        Override: "127.0.0.1" (default) -> "0.0.0.0".
        This is a real override. The server binds to all interfaces, not just
        loopback. This allows connections from remote hosts.
        Note: cfg.peer_ip is NOT overridden in Server.cpp. It retains "127.0.0.1"
        from the default. [OBSERVATION: peer_ip is irrelevant for a server in
        listen mode; TcpBackend [ASSUMPTION] ignores peer_ip when is_server=true.]

    9g. channel_config_default(cfg.channels[0], 0U) called again (line 167).
        Same re-initialization pattern as Client.cpp step 8h.

    9h. cfg.channels[0].reliability = ReliabilityClass::RELIABLE_RETRY (line 168)
        Override: BEST_EFFORT -> RELIABLE_RETRY. Same as client.

    9i. cfg.channels[0].ordering    = OrderingMode::ORDERED             (line 169)
        Override: UNORDERED (default) -> ORDERED.
        KEY DIFFERENCE from client: Server enforces ordered delivery on channel 0.
        Client.cpp uses UNORDERED. [OBSERVATION: This asymmetry means the server
        imposes ordering on received messages even if the client sends unordered.
        The DeliveryEngine [ASSUMPTION] must handle ordering at the receive side.]

    9j. cfg.channels[0].max_retries = 3U                                (line 170)
        Override: MAX_RETRY_COUNT (5U) -> 3U. Same as client.

    9k. cfg.channels[0].recv_timeout_ms = RECV_TIMEOUT_MS               (line 171)
        RECV_TIMEOUT_MS = 100 (Server.cpp line 43).
        Override: 1000U -> 100U. Same value as client (both use 100ms).

---

## 4. Call Tree (Hierarchical)

Client.cpp main()
├── transport_config_default(cfg)               [ChannelConfig.hpp:66]
│   ├── __builtin_memset(&cfg, 0, sizeof cfg)   [<cstring> equivalent]
│   ├── cfg.kind = TCP                          [line 69]
│   ├── cfg.bind_port = 9000                    [line 70]
│   ├── cfg.peer_port = 9000                    [line 71]
│   ├── cfg.connect_timeout_ms = 5000           [line 72]
│   ├── cfg.num_channels = 1                    [line 73]
│   ├── cfg.local_node_id = 1                   [line 74]
│   ├── cfg.is_server = false                   [line 75]
│   ├── [loop] copy "127.0.0.1" to bind_ip     [lines 79-83]
│   ├── [loop] copy "127.0.0.1" to peer_ip     [lines 79-83]
│   └── channel_config_default(channels[0], 0) [line 88]
│       ├── channel_id = 0
│       ├── priority = 0
│       ├── reliability = BEST_EFFORT
│       ├── ordering = UNORDERED
│       ├── max_queue_depth = 64
│       ├── send_timeout_ms = 1000
│       ├── recv_timeout_ms = 1000
│       ├── max_retries = 5
│       ├── retry_backoff_ms = 100
│       └── impairments_enabled = false
├── [override] cfg.kind = TCP                   [Client.cpp:191]
├── [override] cfg.is_server = false            [Client.cpp:192]
├── [override] cfg.peer_port = peer_port        [Client.cpp:193]
├── [override] cfg.connect_timeout_ms = 5000    [Client.cpp:194]
├── [override] cfg.local_node_id = 2            [Client.cpp:195]
├── [override] cfg.num_channels = 1             [Client.cpp:196]
├── [override loop] cfg.peer_ip = argv[1]/"127.0.0.1" [Client.cpp:199-204]
└── channel_config_default(channels[0], 0)      [Client.cpp:207]
    ├── [re-init all defaults]
    ├── [override] reliability = RELIABLE_RETRY [Client.cpp:208]
    ├── [override] ordering = UNORDERED         [Client.cpp:209]
    ├── [override] max_retries = 3              [Client.cpp:210]
    └── [override] recv_timeout_ms = 100        [Client.cpp:211]

Server.cpp main()
├── transport_config_default(cfg)               [ChannelConfig.hpp:66]
│   └── [identical tree as above, omitted for brevity]
├── [override] cfg.kind = TCP                   [Server.cpp:152]
├── [override] cfg.is_server = true             [Server.cpp:153]
├── [override] cfg.bind_port = bind_port        [Server.cpp:154]
├── [override] cfg.local_node_id = 1            [Server.cpp:155]
├── [override] cfg.num_channels = 1             [Server.cpp:156]
├── [override loop] cfg.bind_ip = "0.0.0.0"    [Server.cpp:159-164]
└── channel_config_default(channels[0], 0)      [Server.cpp:167]
    ├── [re-init all defaults]
    ├── [override] reliability = RELIABLE_RETRY [Server.cpp:168]
    ├── [override] ordering = ORDERED           [Server.cpp:169]
    ├── [override] max_retries = 3              [Server.cpp:170]
    └── [override] recv_timeout_ms = 100        [Server.cpp:171]

---

## 5. Key Components Involved

TransportConfig (ChannelConfig.hpp line 37)
    Plain struct. 48-byte bind_ip, 48-byte peer_ip, ports, timeout, num_channels,
    ChannelConfig channels[8], NodeId local_node_id, bool is_server.
    Stack-allocated in both Client.cpp and Server.cpp main() functions.

ChannelConfig (ChannelConfig.hpp line 21)
    Plain struct. 10 fields covering channel identity, reliability, ordering,
    queue depth, timeouts, retries, backoff, and impairment gate.
    Embedded as an array of 8 in TransportConfig.

transport_config_default() (ChannelConfig.hpp line 66)
    Inline. Sets 7 TransportConfig fields + both IP strings + calls
    channel_config_default for channels[0].

channel_config_default() (ChannelConfig.hpp line 51)
    Inline. Sets all 10 ChannelConfig fields. The priority field is derived
    from the channel id: priority = static_cast<uint8_t>(id), making lower
    channel indices higher priority.

TcpBackend (platform/TcpBackend.hpp — not provided)
    Consumes the final TransportConfig in its init() call.
    [ASSUMPTION] Reads is_server, bind_ip, bind_port, peer_ip, peer_port,
    connect_timeout_ms, num_channels, and local_node_id.

DeliveryEngine (core/DeliveryEngine.hpp — not provided)
    Consumes the final ChannelConfig channels[0] in its init() call
    (Client.cpp line 229, Server.cpp line 189).
    [ASSUMPTION] Uses reliability, ordering, max_retries, recv_timeout_ms,
    max_queue_depth, and retry_backoff_ms to configure delivery semantics.

Constants from Types.hpp:
    MSG_RING_CAPACITY = 64U  (max_queue_depth default)
    MAX_RETRY_COUNT   = 5U   (max_retries default)

---

## 6. Branching Logic / Decision Points

In transport_config_default():

Decision 1 — bind_ip / peer_ip copy loop (lines 79-82)
    Condition: lo[i] != '\0' && i < 47
    Loop terminates at null terminator of "127.0.0.1" (9 characters) or at i=47
    (whichever comes first). For "127.0.0.1" the null terminator comes first at i=9.
    Maximum iterations: 47 (bounded by the i < 47 guard — Power of 10 rule 2).

In Client.cpp main():

Decision 2 — argc >= 2 (line 171)
    True  -> peer_ip = argv[1]
    False -> peer_ip = DEFAULT_PEER_IP = "127.0.0.1"

Decision 3 — argc >= 3 (line 174)
    True  -> peer_port = validated atoi(argv[2])
    False -> peer_port = DEFAULT_PEER_PORT = 9000

Decision 4 — argv[2] port range check (line 176)
    Condition: port_val > 0 && port_val <= 65535
    True  -> peer_port = static_cast<uint16_t>(port_val)
    False -> peer_port remains DEFAULT_PEER_PORT = 9000 (no override applied)

Decision 5 — peer_ip copy loop (lines 200-203)
    Condition: peer_ip[i] != '\0' && i < 47
    Same bounded pattern as transport_config_default's loop.

In Server.cpp main():

Decision 6 — argc >= 2 (line 137)
    True  -> bind_port = validated atoi(argv[1])
    False -> bind_port = DEFAULT_BIND_PORT = 9000

Decision 7 — argv[1] port range check (line 139)
    Condition: port_val > 0 && port_val <= 65535
    True  -> bind_port = static_cast<uint16_t>(port_val)
    False -> bind_port remains DEFAULT_BIND_PORT = 9000

Decision 8 — bind_ip copy loop (lines 160-163)
    Condition: DEFAULT_BIND_IP[i] != '\0' && i < 47
    DEFAULT_BIND_IP = "0.0.0.0" (7 characters). Loop runs 7 times.

---

## 7. Concurrency / Threading Behavior

Configuration construction and override occur entirely within the single main()
thread of each process, before TcpBackend::init() and DeliveryEngine::init() are
called. There is no concurrent access to cfg during this phase.

After init() calls complete, cfg is not accessed again (it is a local stack
variable in main). TcpBackend and DeliveryEngine retain their own copies of
relevant config fields [ASSUMPTION].

g_stop_flag in Server.cpp is volatile sig_atomic_t (line 51), but it is not
involved in config initialization; it is a signal-handler communication variable
for the main loop.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

TransportConfig cfg
    Stack-allocated local variable in main() (Client.cpp line 188; Server.cpp line 149).
    Passed by reference to transport_config_default() (which zero-fills it) and
    subsequently to TcpBackend::init() and DeliveryEngine::init().
    No heap allocation. cfg lives for the duration of main().

ChannelConfig channels[MAX_CHANNELS]
    Embedded inline array within TransportConfig. MAX_CHANNELS = 8.
    Total struct size: 2 * 48 (IP strings) + 2 * 2 (ports) + 4 (connect_timeout) +
    4 (num_channels) + 8 * sizeof(ChannelConfig) + 4 (local_node_id) + 1 (is_server)
    + padding. sizeof(ChannelConfig) = at minimum 1+1+1+1+4+4+4+4+4+1 = 25 bytes,
    likely padded to 28 or 32 bytes by the compiler. [ASSUMPTION: no explicit packing.]

__builtin_memset
    GCC/Clang built-in. Called on the entire TransportConfig struct.
    This zeros all 8 ChannelConfig slots, both IP strings, all integers, and the bool.
    The subsequent channel_config_default(channels[0], 0) then correctly initializes
    slot 0. Slots 1-7 remain all-zeros.

Scope of cfg after init():
    cfg is used as an r-value argument to init() calls. TcpBackend and DeliveryEngine
    consume the config during their init phase and should store any fields they need
    internally. cfg is not used after line 229 (Client) / line 189 (Server), though
    it remains alive (on the stack) until main() returns.

---

## 9. Error Handling Flow

transport_config_default() and channel_config_default() are void functions.
They cannot fail. All assignments are unconditional stores to struct fields.
No input validation, no return values, no error paths.

The only conditional logic in the default functions is the loop termination
condition for IP string copy, which is bounded and always terminates.

Error handling for configuration occurs at the call sites in main():

Client.cpp line 217-222:
    res = transport.init(cfg)
    if (!result_ok(res))
        Logger::log FATAL
        return 1;

Server.cpp line 177-182:
    res = transport.init(cfg)
    if (!result_ok(res))
        Logger::log FATAL
        return 1;

If port range validation fails (atoi result out of range), the port variable
simply retains its default value (9000). No error is logged for an invalid port
argument — the silently-retained default is the fallback behavior.
[OBSERVATION: A user providing an invalid port string like "abc" will get
atoi=0, which fails the > 0 check, and the default port is silently used.
This could be confusing. No WARNING is logged for this case.]

---

## 10. External Interactions

__builtin_memset (line 68 of ChannelConfig.hpp)
    GCC/Clang compiler built-in. Equivalent to memset but potentially expanded
    inline. The (void) cast discards the return. This is the only non-trivial
    external interaction during config construction.

atoi (Client.cpp lines 175, 176; Server.cpp lines 138, 139)
    C standard library. Used to parse command-line port arguments. atoi does not
    set errno and returns 0 on parse failure, making error detection reliant on
    the range check (> 0 && <= 65535).

No socket calls, no file I/O, no logging during the config construction phase.
Logging begins after transport_config_default() and the override section complete,
at the TcpBackend::init() result check.

---

## 11. State Changes / Side Effects

transport_config_default(cfg) writes:
    cfg (entire struct, 0-filled then field-set)
        cfg.kind               = TCP (0)
        cfg.bind_ip            = "127.0.0.1\0..."
        cfg.bind_port          = 9000
        cfg.peer_ip            = "127.0.0.1\0..."
        cfg.peer_port          = 9000
        cfg.connect_timeout_ms = 5000
        cfg.num_channels       = 1
        cfg.local_node_id      = 1
        cfg.is_server          = false
        cfg.channels[0]:
            channel_id          = 0
            priority            = 0
            reliability         = BEST_EFFORT (0)
            ordering            = UNORDERED (1)
            max_queue_depth     = 64
            send_timeout_ms     = 1000
            recv_timeout_ms     = 1000
            max_retries         = 5
            retry_backoff_ms    = 100
            impairments_enabled = false
        cfg.channels[1..7]:  all-zeros (not valid ChannelConfig objects)

Client.cpp final cfg after all overrides:
    cfg.kind               = TCP (unchanged)
    cfg.bind_ip            = "127.0.0.1" (unchanged)
    cfg.bind_port          = 9000 (unchanged; client does not set bind_port explicitly)
    cfg.peer_ip            = argv[1] or "127.0.0.1"
    cfg.peer_port          = argv[2] or 9000
    cfg.connect_timeout_ms = 5000 (unchanged)
    cfg.num_channels       = 1 (unchanged)
    cfg.local_node_id      = 2  [KEY CHANGE: 1 -> 2]
    cfg.is_server          = false (unchanged)
    cfg.channels[0]:
        channel_id          = 0 (re-defaulted then unchanged)
        priority            = 0 (re-defaulted)
        reliability         = RELIABLE_RETRY  [KEY CHANGE: BEST_EFFORT -> RELIABLE_RETRY]
        ordering            = UNORDERED (re-defaulted, unchanged)
        max_queue_depth     = 64 (re-defaulted, unchanged)
        send_timeout_ms     = 1000 (re-defaulted, unchanged)
        recv_timeout_ms     = 100  [KEY CHANGE: 1000 -> 100]
        max_retries         = 3    [KEY CHANGE: 5 -> 3]
        retry_backoff_ms    = 100 (re-defaulted, unchanged)
        impairments_enabled = false (re-defaulted, unchanged)

Server.cpp final cfg after all overrides:
    cfg.kind               = TCP (unchanged)
    cfg.bind_ip            = "0.0.0.0"  [KEY CHANGE: "127.0.0.1" -> "0.0.0.0"]
    cfg.bind_port          = argv[1] or 9000 (unchanged if default)
    cfg.peer_ip            = "127.0.0.1" (unchanged; irrelevant for server)
    cfg.peer_port          = 9000 (unchanged)
    cfg.connect_timeout_ms = 5000 (unchanged)
    cfg.num_channels       = 1 (unchanged)
    cfg.local_node_id      = 1 (unchanged; server claims node ID 1)
    cfg.is_server          = true  [KEY CHANGE: false -> true]
    cfg.channels[0]:
        channel_id          = 0 (re-defaulted)
        priority            = 0 (re-defaulted)
        reliability         = RELIABLE_RETRY  [KEY CHANGE: BEST_EFFORT -> RELIABLE_RETRY]
        ordering            = ORDERED  [KEY CHANGE: UNORDERED -> ORDERED; different from client]
        max_queue_depth     = 64 (re-defaulted, unchanged)
        send_timeout_ms     = 1000 (re-defaulted, unchanged)
        recv_timeout_ms     = 100  [KEY CHANGE: 1000 -> 100]
        max_retries         = 3    [KEY CHANGE: 5 -> 3]
        retry_backoff_ms    = 100 (re-defaulted, unchanged)
        impairments_enabled = false (re-defaulted, unchanged)

---

## 12. Sequence Diagram (ASCII)

    main()               transport_config_default()    channel_config_default()    [overrides]
      |                          |                             |                        |
      |--transport_config_default(cfg)-->                      |                        |
      |                          |--__builtin_memset(cfg,0)    |                        |
      |                          |--cfg.kind=TCP               |                        |
      |                          |--cfg.bind_port=9000         |                        |
      |                          |--cfg.peer_port=9000         |                        |
      |                          |--cfg.connect_timeout=5000   |                        |
      |                          |--cfg.num_channels=1         |                        |
      |                          |--cfg.local_node_id=1        |                        |
      |                          |--cfg.is_server=false        |                        |
      |                          |--[loop] bind_ip="127.0.0.1" |                        |
      |                          |--[loop] peer_ip="127.0.0.1" |                        |
      |                          |--channel_config_default(ch0)-->                      |
      |                          |                             |--ch.channel_id=0       |
      |                          |                             |--ch.priority=0         |
      |                          |                             |--ch.reliability=BE     |
      |                          |                             |--ch.ordering=UNORDERED |
      |                          |                             |--ch.max_queue_depth=64 |
      |                          |                             |--ch.send_timeout=1000  |
      |                          |                             |--ch.recv_timeout=1000  |
      |                          |                             |--ch.max_retries=5      |
      |                          |                             |--ch.backoff=100        |
      |                          |                             |--ch.impair=false       |
      |<--(returns)--------------+-----------------------------+                        |
      |                                                                                 |
      | [Client only]                                                                   |
      |--cfg.local_node_id=2----------------------------------------------------->     |
      |--cfg.peer_port=peer_port------------------------------------------------->     |
      |--[loop] cfg.peer_ip=argv[1]----------------------------------------------> |
      |--channel_config_default(ch0) [re-init]------------------------------------>    |
      |                                                        channel_config_default() |
      |                                                             [re-applies defaults]|
      |--cfg.channels[0].reliability=RELIABLE_RETRY----------------------------->      |
      |--cfg.channels[0].max_retries=3------------------------------------------>     |
      |--cfg.channels[0].recv_timeout_ms=100------------------------------------>      |
      |                                                                                 |
      | [Server only]                                                                   |
      |--cfg.is_server=true------------------------------------------------------>     |
      |--cfg.bind_port=bind_port------------------------------------------------->     |
      |--[loop] cfg.bind_ip="0.0.0.0"-------------------------------------------> |
      |--channel_config_default(ch0) [re-init]------------------------------------>    |
      |--cfg.channels[0].reliability=RELIABLE_RETRY----------------------------->      |
      |--cfg.channels[0].ordering=ORDERED--------------------------------------->      |
      |--cfg.channels[0].max_retries=3------------------------------------------>     |
      |--cfg.channels[0].recv_timeout_ms=100------------------------------------>      |

---

## 13. Initialization vs Runtime Flow

Initialization phase (one-time, in main() before the send/receive loop):
    1. transport_config_default() populates cfg with safe defaults.
    2. Per-app overrides are applied inline in main().
    3. channel_config_default() is called a second time to reset channels[0]
       before channel-specific overrides.
    4. TcpBackend::init(cfg) consumes the final config.
    5. DeliveryEngine::init(&transport, cfg.channels[0], node_id) consumes channel config.

After these init calls, cfg is not consulted again. TcpBackend and DeliveryEngine
retain whatever fields they need internally. The TransportConfig struct on the
stack is effectively read-only from the perspective of the main loop.

Runtime phase:
    Config has no runtime role. No config fields are read or modified during
    the send/receive loops. Changes to impairment, retry, or timeout behavior
    after init would require a re-init sequence (not implemented in these files).

---

## 14. Known Risks / Observations

Risk 1 — __builtin_memset is non-standard
    ChannelConfig.hpp line 68 uses __builtin_memset, a GCC/Clang extension.
    MSVC and other conforming C++17 compilers do not provide this built-in.
    The standard <cstring> memset is portable and would be MISRA-compliant here.
    This is a portability deviation without documented justification.

Risk 2 — channels[1..7] left as all-zeros after transport_config_default()
    Only channels[0] is initialized by transport_config_default(). If num_channels
    is later increased to > 1 without calling channel_config_default() for each
    additional channel, DeliveryEngine or TcpBackend would consume zero-filled
    ChannelConfig objects. This would produce a channel with channel_id=0 (collision
    with channel 0), priority=0, BEST_EFFORT, max_queue_depth=0, and all timeouts=0.
    There is no runtime check that num_channels matches the number of properly
    initialized channel slots.

Risk 3 — channel_config_default() called twice in each app
    Both Client.cpp and Server.cpp call channel_config_default(channels[0], 0)
    inside transport_config_default() (via line 88) and then again explicitly
    (Client.cpp line 207; Server.cpp line 167) before applying overrides.
    The second call re-applies all defaults, erasing any values that the first
    call (within transport_config_default) set. This is harmless but redundant.
    If transport_config_default() were extended to set non-default channel values,
    the second explicit call would silently reset them.

Risk 4 — Silent fallback on invalid port argument
    If atoi() returns 0 (invalid input like "abc") or the value is out of range
    (e.g., 99999), the port override is silently skipped and the default 9000
    is used. No WARNING or INFO log is emitted. A user who passes an invalid port
    on the command line would see the program connect on port 9000 without notice.

Risk 5 — Client does not override bind_ip; Server does not override peer_ip
    Client.cpp never sets cfg.bind_ip; it retains "127.0.0.1" from the default.
    Server.cpp never sets cfg.peer_ip; it retains "127.0.0.1".
    Whether these unused fields cause problems depends on TcpBackend's behavior
    [ASSUMPTION: TcpBackend ignores bind_ip for TCP clients and ignores peer_ip
    for TCP servers]. If TcpBackend always binds to bind_ip (even for clients),
    the client would be bound to the loopback interface, preventing connections
    from routing through non-loopback interfaces.

Risk 6 — OrderingMode asymmetry between client and server
    Client channel 0: UNORDERED. Server channel 0: ORDERED.
    This means the DeliveryEngine on the server side is configured to enforce
    ordering, while the client makes no such guarantee. If the DeliveryEngine's
    ordered mode requires sequence numbers to be present and the client sends
    unordered messages without them, the server may fail to deliver or may
    stall waiting for a missing sequence. [ASSUMPTION: DeliveryEngine handles
    this gracefully, but the asymmetry is a potential correctness issue.]

Risk 7 — local_node_id default is 1, same as SERVER_NODE_ID
    transport_config_default sets local_node_id = 1. Server.cpp uses
    LOCAL_SERVER_NODE_ID = 1 and does not change it from the default, so the
    override is a no-op. Client.cpp changes it to 2. If a third peer were added
    with the default config and no override, it would claim node_id = 1 (colliding
    with the server). Node ID uniqueness is not enforced anywhere in the
    configuration layer.

---

## 15. Unknowns / Assumptions

[ASSUMPTION-1] TcpBackend::init() copies all fields it needs from TransportConfig
    into its own internal state. If TcpBackend retains a pointer to cfg rather
    than copying, cfg's stack lifetime would need to exceed TcpBackend's operational
    lifetime. Given that cfg is a local in main(), this would be satisfied, but it
    is an implicit contract not visible in these files.

[ASSUMPTION-2] DeliveryEngine::init() similarly copies or references cfg.channels[0]
    for the duration of the engine's operation.

[ASSUMPTION-3] TcpBackend uses cfg.peer_ip and cfg.peer_port to establish an
    outgoing TCP connection (client mode) and uses cfg.bind_ip and cfg.bind_port
    to bind and listen (server mode). This is the standard TCP client/server
    distinction, but TcpBackend source is not provided.

[ASSUMPTION-4] OrderingMode::ORDERED on the server's channel configures the
    DeliveryEngine to buffer and re-sequence incoming messages. This is inferred
    from the application requirements (CLAUDE.md §3.3) but DeliveryEngine source
    is not provided.

[ASSUMPTION-5] impairments_enabled = false in both configurations means the
    ImpairmentEngine is entirely bypassed for channel 0. If an impairment engine
    is wired in and checks this flag per-channel, no artificial latency, jitter,
    loss, or duplication will be applied.

[ASSUMPTION-6] The priority field in ChannelConfig (set to 0 for channel 0 by
    default, meaning highest priority) is consumed by some channel-scheduling
    mechanism in DeliveryEngine or TcpBackend. With only 1 channel configured
    in both Client and Server, priority scheduling is moot in the current setup.

Unknown-1: sizeof(TransportConfig) and sizeof(ChannelConfig). Compiler alignment
    rules may insert padding. The __builtin_memset zero-fills the entire struct
    including padding, which is correct, but the exact sizes affect stack usage.

Unknown-2: Whether TcpBackend enforces the cfg.num_channels limit against the
    actual number of channel slots it processes. If TcpBackend iterates
    channels[0..num_channels-1] without further bounds checking, increasing
    num_channels beyond the number of properly initialized channels (risk 2) would
    cause silent misuse of zero-filled ChannelConfig slots.

Unknown-3: How send_timeout_ms (left at 1000ms default in both Client and Server
    after channel re-initialization) interacts with DeliveryEngine's retry_backoff_ms
    (100ms) and max_retries (3). The relationship between send_timeout, retry
    backoff, and the overall message expiry_time_us is defined at the application
    layer (5-second expiry in Client, 10-second in Server) but the exact precedence
    order is not determinable from these files alone.agentId: a06cdaac5c7b9151c (for resuming to continue this agent's work if needed)
<usage>total_tokens: 58802
tool_uses: 7
duration_ms: 406303</usage>