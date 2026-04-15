# Transport Specification — Abstraction API and Platform Backends

**Normative REQ IDs:** defined in `CLAUDE.md §§4 and 6`. This document contains the full prose specification and rationale. When a conflict exists, the REQ ID lines in `CLAUDE.md` take precedence.
**Last updated:** 2026-04-15

---

## 4. Transport Abstraction API

### 4.1 Core Operations

The `TransportInterface` (declared in `src/core/TransportInterface.hpp`) exposes:

| Method | REQ | Description |
|---|---|---|
| `init(config)` | REQ-4.1.1 | Initialize transport and impairment state; allocate any fixed resources here |
| `send_message(envelope)` | REQ-4.1.2 | Queue or send a single message envelope via an appropriate channel |
| `receive_message(timeout)` | REQ-4.1.3 | Block or poll until a message envelope is received or timeout elapses |
| `flush()` / `close()` | REQ-4.1.4 | Gracefully flush pending messages and close all resources |

All five backends (`TcpBackend`, `UdpBackend`, `TlsTcpBackend`, `DtlsUdpBackend`, `LocalSimHarness`) implement `TransportInterface` and are interchangeable at the `DeliveryEngine` level.

### 4.2 Configuration and Channels

**Logical channels** (`ChannelConfig` in `src/core/ChannelConfig.hpp`) carry:
- Priority level — used for internal queueing decisions
- Reliability mode — `BEST_EFFORT` or `RELIABLE`
- Ordering requirement — `ORDERED` or `UNORDERED`

**Transport configuration** (`TransportConfig` in `src/core/TransportConfig.hpp`) allows:
- Send, receive, and connect timeouts
- Per-channel queue depth and rate limits
- Which impairments apply to which channels

Call `transport_config_default()` and `channel_config_default()` to initialize structs to safe defaults before overriding individual fields.

---

## 6. Platform / Network Backend Requirements

### 6.1 TCP (Connection-Oriented) Backend

**Header:** `src/platform/TcpBackend.hpp` (plaintext), `src/platform/TlsTcpBackend.hpp` (TLS)

#### Connection Management
- Standard 3-way handshake (REQ-6.1.1).
- Connection timeout is configurable via `TransportConfig::connect_timeout_ms` (REQ-6.1.2).
- Graceful disconnect via FIN/ACK; abrupt disconnect (RST, ECONNRESET) is detected and logged (REQ-6.1.3).

#### Reliability and Framing
- TCP delivers bytes in order; application-level failures (timeouts, lost connections) are still detected.
- Messages are framed with a 4-byte length prefix (REQ-6.1.5). The receiver must buffer partial reads and reassemble complete frames before passing to the deserializer (REQ-6.1.6).
- Application-level flow control via per-connection send queues bounded by `TransportConfig::send_queue_depth` (REQ-6.1.4).

#### Concurrency
- Supports `MAX_TCP_CONNECTIONS` simultaneous client slots (REQ-6.1.7). The server uses a non-blocking accept loop with `select()`/`poll()`.

#### HELLO Registration (REQ-6.1.8)
When a TCP or TLS client establishes a connection, it must send a `HELLO` frame before any `DATA` frame:
- `message_type` = `MessageType::HELLO` (= 4)
- `source_id` = the client's local `NodeId`
- `payload_length` = 0

The server records the `NodeId → socket-slot` mapping when it receives the `HELLO`. Until the `HELLO` is received, the slot is treated as unregistered.

#### Server-Side Unicast Routing (REQ-6.1.9)
The server maintains a `NodeId → socket-slot` routing table populated from received `HELLO` frames.
- `destination_id != NODE_ID_INVALID (0)` → route to the single matching slot; return `ERR_INVALID` + `WARNING_HI` if no slot matches.
- `destination_id == 0` → fan-out to all connected clients (broadcast).

#### TransportInterface::register_local_id() (REQ-6.1.10)
`TransportInterface` exposes `virtual Result register_local_id(NodeId id)` with a default body that returns `OK`. Overrides:
- TCP/TLS client: sends `HELLO` frame on-wire.
- TCP/TLS server: stores `local_node_id` for reference.
- UDP/DTLS backends: store `local_node_id` and send a `HELLO` datagram to the configured peer (UDP always; DTLS client only — server has no connected peer at registration time).
- `LocalSimHarness`: inherits the default no-op.

`DeliveryEngine::init()` calls `register_local_id(local_id)` immediately after `transport->init()` returns `OK`. Any failure is logged at `WARNING_HI`.

#### Source Address Validation (REQ-6.1.11)
Before passing any deserialized envelope to `DeliveryEngine::receive()`, the TCP and TLS backends verify that the wire-level source address (socket slot) is consistent with the `source_id` field in the envelope. A mismatch results in silent discard and a `WARNING_HI` log entry. The mismatched envelope never reaches `DeliveryEngine`. Rationale: prevents `source_id` spoofing attacks that could corrupt ordering state, trigger spurious ACKs, or exhaust dedup window slots. See `docs/SECURITY_ASSUMPTIONS.md §4`.

#### HELLO Timeout — Slow-Connect Slot Eviction (REQ-6.1.12)
The TCP and TLS servers track a per-slot accept timestamp. Any client slot that has not sent a `HELLO` within `hello_timeout_ms` (default: `recv_timeout_ms`) of acceptance is evicted: socket is closed, `WARNING_HI` is logged, and the slot is freed. Rationale: prevents slot-exhaustion DoS via slow-connect (H-5 from SEC_REPORT.txt; HAZ-023; CWE-400).

---

### 6.2 UDP (Connectionless) Backend

**Header:** `src/platform/UdpBackend.hpp` (plaintext), `src/platform/DtlsUdpBackend.hpp` (DTLS)

- No inherent reliability (REQ-6.2.1). Optional ACK/NAK + retry is layered above via `AckTracker` and `RetryManager`.
- Sequence numbers are used when ordering is required; `OrderingBuffer` handles reassembly (REQ-6.2.2).
- UDP datagrams are bounded by MTU; large messages are fragmented at the message layer via `send_fragments()` (REQ-6.2.3).

#### Source Address Validation (REQ-6.2.4)
Each received datagram's wire-level source address is compared against the `source_id` field in the deserialized envelope. Mismatches result in silent discard and `WARNING_HI`. This is the UDP equivalent of REQ-6.1.11.

#### Wildcard Peer IP Rejection (REQ-6.2.5)
`UdpBackend::init()` rejects a wildcard `peer_ip` (`"0.0.0.0"`, `"::"`, or empty string) with `ERR_INVALID` and logs `FATAL`. A specific peer address is required for source-address binding to be effective. Rationale: HAZ-024, CWE-290.

---

### 6.3 Common Socket Requirements (TCP and UDP)

- Bind to configurable IP/port pairs; IPv4 required, IPv6 optional; `SO_REUSEADDR` used to avoid startup failures (REQ-6.3.1).
- All error codes explicitly handled; classified as recoverable (`WARNING_LO` / `WARNING_HI`) or fatal (`FATAL`) (REQ-6.3.2).
- Read/write timeouts set via `setsockopt(SO_RCVTIMEO / SO_SNDTIMEO)` (REQ-6.3.3).
- `TransportInterface` provides the extension point for TLS/DTLS; secure and insecure backends are interchangeable at the `DeliveryEngine` level (REQ-6.3.4).
- All buffer sizes are compile-time constants from `src/core/Types.hpp`; no unbounded growth (REQ-6.3.5).

#### TLS/DTLS Configuration Validation

**CA Certificate (REQ-6.3.6)**
When `verify_peer=true`, a non-empty `ca_file` is mandatory. `init()` returns `ERR_IO` + `FATAL` if `ca_file` is empty. Rationale: without a trust anchor, certificate verification is meaningless (H-1; HAZ-020; CWE-295).

**CRL Enforcement (REQ-6.3.7)**
When `require_crl=true` and `verify_peer=true`, `crl_file` must be non-empty. `init()` returns `ERR_INVALID` + `FATAL` if `crl_file` is empty. Default: `require_crl=false` for backward compatibility (H-2; CWE-295).

**Forward Secrecy (REQ-6.3.8)**
When `tls_require_forward_secrecy=true`, any TLS 1.2 session resumption handshake is rejected after negotiation: the session is zeroized and `ERR_IO` is returned. Default: `false` for backward compatibility (H-4; CWE-295).

**Hostname / Peer-Verify Mismatch (REQ-6.3.9)**
When `verify_peer=false` and `peer_hostname` is non-empty, `init()` returns `ERR_INVALID` + `WARNING_HI`. This combination is an unsafe operator configuration that must not be reachable silently (H-8; HAZ-025; CWE-297).

**TlsSessionStore Concurrent-Access Mutex (REQ-6.3.10)**
`TlsSessionStore` in `src/platform/TlsSessionStore.hpp` protects its internal `mbedtls_ssl_session` struct with a POSIX mutex. The mutex guards all accesses from `zeroize()`, `try_save()`, and `try_load_client_session()`. The `std::atomic<bool> session_valid` flag alone is insufficient because it does not protect the struct contents (H-3; HAZ-021; CWE-362).

---

### 6.4 DTLS (Datagram TLS over UDP) Backend

**Header:** `src/platform/DtlsUdpBackend.hpp`

**DTLS Mode (REQ-6.4.1)**
Uses `MBEDTLS_SSL_TRANSPORT_DATAGRAM`. Supports both client and server roles. When `tls_enabled=false`, falls through to plain UDP socket path with no handshake (REQ-6.4.5).

**Anti-Replay and Cookie Exchange (REQ-6.4.2)**
The DTLS server arms cookie exchange via `mbedtls_ssl_cookie_setup()`. Each cookie is bound to the client's `(address, port)` transport identity. Cookies prevent UDP amplification attacks where a spoofed source causes the server to flood a third party.

**Retransmission Timer (REQ-6.4.3)**
`mbedTLS` timer callbacks (`mbedtls_ssl_set_timer_cb`) drive DTLS retransmission without busy-waiting. The timer min/max values are set to match the expected link RTT via `DtlsUdpBackend` configuration.

**MTU Enforcement (REQ-6.4.4)**
Outbound messages whose serialized size exceeds `DTLS_MAX_DATAGRAM_BYTES` are rejected before encryption. This prevents IP fragmentation of DTLS records, which is not supported by all middleboxes.

**Plaintext Fallback (REQ-6.4.5)**
When `tls_enabled=false`, `DtlsUdpBackend` skips the handshake and certificate loading and operates as a plain UDP socket. This allows the same backend code to be used for testing without changing higher layers.

**DTLS Peer Hostname Verification (REQ-6.4.6)**
When `verify_peer=true` and `peer_hostname` is non-empty, `client_connect_and_handshake()` calls `mbedtls_ssl_set_hostname()` after `ssl_setup` and before the handshake. This binds the expected certificate CN/SAN. Failure to bind returns `ERR_IO`. When `peer_hostname` is empty, `nullptr` is passed — the opt-out is explicit.
