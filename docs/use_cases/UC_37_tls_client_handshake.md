# UC_37 — TLS client: connect to peer and complete TLS handshake before first message

**HL Group:** HL-18 — TLS-Encrypted TCP Transport
**Actor:** System (invoked during User's call to `TlsTcpBackend::init()`)
**Requirement traceability:** REQ-4.1.1, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.3.4, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** User calls `TlsTcpBackend::init()` with `config.is_server = false` and `config.tls.tls_enabled = true`.

**Goal:** Configure the shared mbedTLS ssl_conf object (loading own certificate, private key, and optionally a CA for peer verification), establish a TCP connection to the configured peer IP and port, and complete a TLS 1.2+ handshake before `init()` returns, making the transport immediately usable for framed send/receive.

**Success outcome:** `init()` returns `Result::OK` with `m_open = true`, `m_client_count = 1`, `m_client_net[0].fd` holding a valid connected socket, and `m_ssl[0]` holding a fully established TLS session.

**Error outcomes:**
- `psa_crypto_init()` fails → `init()` returns `Result::ERR_IO`; `m_open` remains false.
- Any cert/key file fails to load → `Result::ERR_IO`.
- `mbedtls_net_connect()` fails (no server, refused) → `Result::ERR_IO`.
- `mbedtls_net_set_block()` fails → client fd freed; `Result::ERR_IO`.
- `mbedtls_ssl_setup()` fails → client fd freed; `Result::ERR_IO`.
- `mbedtls_ssl_handshake()` fails → ssl context and client fd freed and re-initialised; `Result::ERR_IO`.

---

## 2. Entry Points

**Primary (User-facing):**
```
// src/platform/TlsTcpBackend.cpp, line 653
Result TlsTcpBackend::init(const TransportConfig& config)
```

**Internal sub-entry (called unconditionally by init when is_server == false):**
```
// src/platform/TlsTcpBackend.cpp, line 259
Result TlsTcpBackend::connect_to_server()
```

---

## 3. End-to-End Control Flow (Step-by-Step)

1. `TlsTcpBackend::init(config)` is called. Assertions verify `config.kind == TransportKind::TCP` and `!m_open`.
2. `m_cfg`, `m_is_server` (`false`), and `m_tls_enabled` (`true`) are stored.
3. `m_recv_queue.init()` resets the inbound ring buffer to empty.
4. `impairment_config_default(imp_cfg)` populates safe defaults; if `config.num_channels > 0`, channel 0's `ImpairmentConfig` is used.
5. `m_impairment.init(imp_cfg)` initialises the impairment engine.
6. Because `m_tls_enabled == true`, `setup_tls_config(config.tls)` is called.
7. Inside `setup_tls_config()`:
   a. Assertion: `tls_cfg.tls_enabled == true`; `tls_cfg.cert_file[0] != '\0'`.
   b. `psa_crypto_init()` — initialises PSA Crypto DRBG (init-phase heap allocation; Power of 10 Rule 3 deviation).
   c. `endpoint = MBEDTLS_SSL_IS_CLIENT` (because `tls_cfg.role == TlsRole::CLIENT`).
   d. `mbedtls_ssl_config_defaults(&m_ssl_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)`.
   e. `mbedtls_ssl_conf_authmode()` → `MBEDTLS_SSL_VERIFY_REQUIRED` if `verify_peer`, else `MBEDTLS_SSL_VERIFY_NONE`.
   f. Conditional CA load: if `verify_peer && ca_file[0] != '\0'` → `mbedtls_x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file)` then `mbedtls_ssl_conf_ca_chain(&m_ssl_conf, &m_ca_cert, nullptr)`.
   g. `mbedtls_x509_crt_parse_file(&m_cert, tls_cfg.cert_file)` — loads own certificate (may be a chain).
   h. `mbedtls_pk_parse_keyfile(&m_pkey, tls_cfg.key_file, nullptr)` — loads own private key.
   i. `mbedtls_ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey)` — binds cert + key to the shared config.
   j. INFO log: "TLS config ready: role=CLIENT".
   k. Returns `Result::OK`.
8. Back in `init()`: because `m_is_server == false`, `connect_to_server()` is called.
9. Inside `connect_to_server()`:
   a. Assertions: `!m_is_server`; `m_client_net[0U].fd < 0` (slot is free).
   b. `port_to_str(m_cfg.peer_port, port_str)` — convert `uint16_t` port to decimal string (6-byte stack buffer).
   c. `mbedtls_net_connect(&m_client_net[0U], m_cfg.peer_ip, port_str, MBEDTLS_NET_PROTO_TCP)` — performs DNS resolution (if needed), creates the socket, sets `SO_REUSEADDR`, calls `connect()`. Returns 0 on success; a blocking TCP connection is established.
   d. Because `m_tls_enabled == true`:
      i. `mbedtls_net_set_block(&m_client_net[0U])` — ensures the fd is in blocking mode before the handshake (explicit for safety even though `net_connect` returns a blocking socket).
      ii. `mbedtls_ssl_setup(&m_ssl[0U], &m_ssl_conf)` — initialises the TLS session from the shared config.
      iii. `mbedtls_ssl_set_bio(&m_ssl[0U], &m_client_net[0U], mbedtls_net_send, mbedtls_net_recv, nullptr)` — wires the TLS BIO to slot 0's net context.
      iv. `mbedtls_ssl_handshake(&m_ssl[0U])` — drives the full TLS client handshake:
          - Sends ClientHello.
          - Receives ServerHello, Certificate, ServerHelloDone.
          - Sends ClientKeyExchange, ChangeCipherSpec, Finished.
          - Receives server ChangeCipherSpec and Finished.
          - Returns 0 when the handshake is complete.
          - This call blocks on the caller's thread (init-phase; Power of 10 Rule 3 deviation for mbedTLS internal allocations).
      v. On handshake failure: `mbedtls_ssl_free(&m_ssl[0U])`, `mbedtls_ssl_init(&m_ssl[0U])`, `mbedtls_net_free(&m_client_net[0U])`, `mbedtls_net_init(&m_client_net[0U])`; return `Result::ERR_IO`.
      vi. On success: INFO log with negotiated cipher suite (`mbedtls_ssl_get_ciphersuite(&m_ssl[0U])`).
   e. `m_client_count = 1U`.
   f. `m_open = true`.
   g. INFO log: "Connected to <peer_ip>:<peer_port> (TLS=ON)".
   h. Assertions: `m_client_net[0U].fd >= 0`; `m_client_count == 1U`.
   i. Returns `Result::OK`.
10. Back in `init()`: assertion `m_open`; returns `Result::OK`.

---

## 4. Call Tree (Primary Success Path)

```
TlsTcpBackend::init()
 ├── m_recv_queue.init()
 ├── impairment_config_default()
 ├── m_impairment.init()
 ├── setup_tls_config()
 │    ├── psa_crypto_init()                         [PSA Crypto — init-phase heap]
 │    ├── mbedtls_ssl_config_defaults()             [STREAM, CLIENT, PRESET_DEFAULT]
 │    ├── mbedtls_ssl_conf_authmode()
 │    ├── mbedtls_x509_crt_parse_file()  (CA cert, conditional)
 │    ├── mbedtls_ssl_conf_ca_chain()   (conditional)
 │    ├── mbedtls_x509_crt_parse_file()  (own cert)
 │    ├── mbedtls_pk_parse_keyfile()
 │    └── mbedtls_ssl_conf_own_cert()
 └── connect_to_server()
      ├── port_to_str()
      ├── mbedtls_net_connect()                     [DNS + socket + connect()]
      ├── mbedtls_net_set_block()                   [explicit blocking mode]
      ├── mbedtls_ssl_setup()
      ├── mbedtls_ssl_set_bio()
      └── mbedtls_ssl_handshake()                   [full TLS handshake; blocking]
```

---

## 5. Key Components Involved

- **`TlsTcpBackend`** — holds all mbedTLS contexts as fixed member arrays; the single `m_ssl[0]` and `m_client_net[0]` hold the client-mode TLS session.
- **`setup_tls_config()`** — configures the shared `mbedtls_ssl_config`; performs all file I/O for cert and key material; called once during `init()`.
- **`connect_to_server()`** — establishes the TCP connection and drives the TLS handshake to completion; called once during `init()`.
- **`port_to_str()`** — file-local helper; converts `uint16_t` port to NUL-terminated decimal string on a 6-byte stack buffer; used by both connect and bind paths.
- **`log_mbedtls_err()`** — file-local helper; calls `mbedtls_strerror()` into a 128-byte stack buffer and emits WARNING_HI.
- **`mbedtls_ssl_config`** (`m_ssl_conf`) — shared TLS config; populated once in `setup_tls_config()`; must not be modified after the first `ssl_setup()` call.
- **`mbedtls_net_context`** (`m_client_net[0]`) — wraps the connected POSIX fd; used by BIO callbacks.
- **`mbedtls_ssl_context`** (`m_ssl[0]`) — TLS session for the single outbound connection.
- **`ImpairmentEngine`** — initialised but not exercised until `send_message()` / `receive_message()`.
- **`RingBuffer`** (`m_recv_queue`) — reset to empty; used only after `init()` returns successfully.

---

## 6. Branching Logic / Decision Points

| Condition | True path | False path |
|-----------|-----------|------------|
| `m_tls_enabled` in `init()` | Call `setup_tls_config()` | Skip; go directly to `connect_to_server()` |
| `tls_cfg.verify_peer && ca_file[0] != '\0'` | Parse CA cert; register ca_chain | Skip CA cert |
| `psa_crypto_init() != PSA_SUCCESS` | Log FATAL; return `ERR_IO` | Continue |
| Any `mbedtls_ssl_config_defaults()` / cert / key call fails | Log WARNING_HI; return `ERR_IO` | Continue |
| `mbedtls_net_connect()` fails | Log WARNING_HI; return `ERR_IO` | Proceed to `net_set_block()` |
| `m_tls_enabled` in `connect_to_server()` | Call `net_set_block`, `ssl_setup`, `ssl_set_bio`, `ssl_handshake` | Skip TLS; set `m_client_count=1`, `m_open=true`; return OK |
| `mbedtls_net_set_block()` fails | Free client fd; return `ERR_IO` | Call `ssl_setup()` |
| `mbedtls_ssl_setup()` fails | Free client fd; return `ERR_IO` | Call `ssl_set_bio()` + `ssl_handshake()` |
| `mbedtls_ssl_handshake()` fails | ssl_free + ssl_init + net_free + net_init; return `ERR_IO` | Set `m_client_count=1`, `m_open=true`; return OK |

---

## 7. Concurrency / Threading Behavior

All operations execute on the single thread that calls `init()`. No threads are created by `TlsTcpBackend`.

The `mbedtls_ssl_handshake()` call blocks on the caller's thread for the full round-trip duration of the TLS handshake. The caller must ensure that the server is reachable and responsive; there is no application-level connection timeout wrapping this call (mbedTLS internal socket read/write timeouts apply if the socket has been configured with one, but none is set explicitly here).

No `std::atomic`, mutex, or other synchronisation primitive is used in this flow.

---

## 8. Memory and Ownership Semantics

**Stack allocations in `connect_to_server()`:**
- `char port_str[6]` — decimal port string.

**Stack allocations in `log_mbedtls_err()`:**
- `char err_buf[128]` — mbedTLS error string buffer.

**mbedTLS internal heap allocations (init-phase, Power of 10 Rule 3 deviation):**
- `psa_crypto_init()` — PSA Crypto DRBG state.
- `mbedtls_x509_crt_parse_file()` — parsed DER bytes for `m_cert` and `m_ca_cert`.
- `mbedtls_pk_parse_keyfile()` — parsed key material in `m_pkey`.
- `mbedtls_ssl_handshake()` — handshake state machine and record buffers in `m_ssl[0]`.
- None of the above occur on the send/receive critical path.

**Fixed member allocations (embedded in `TlsTcpBackend` object, Power of 10 Rule 3):**
- `m_ssl_conf`, `m_cert`, `m_ca_cert`, `m_pkey`, `m_listen_net` — shared TLS config contexts.
- `m_client_net[0]` — net context for the single outbound connection.
- `m_ssl[0]` — TLS session for the single outbound connection.
- `m_wire_buf[8192]` — serialization/framing buffer.

**Ownership and cleanup:**
- On `ssl_handshake()` failure, the partially initialised slot is fully freed before returning: `mbedtls_ssl_free()`, `mbedtls_ssl_init()`, `mbedtls_net_free()`, `mbedtls_net_init()`.
- The destructor calls `close()` then frees all contexts and calls `mbedtls_psa_crypto_free()`.
- `m_sock_ops` is a non-owning pointer; it must outlive `TlsTcpBackend`.

**`SOCKET_RECV_BUF_BYTES` = 8192. `MAX_TCP_CONNECTIONS` = 8; client mode uses only slot 0.**

---

## 9. Error Handling Flow

| Error event | State after | `init()` return value |
|-------------|-------------|----------------------|
| `psa_crypto_init()` fails | `m_open = false` | `ERR_IO` |
| `ssl_config_defaults()` fails | `m_open = false` | `ERR_IO` |
| CA cert parse fails | `m_open = false` | `ERR_IO` |
| Own cert parse fails | `m_open = false` | `ERR_IO` |
| Key file parse fails | `m_open = false` | `ERR_IO` |
| `ssl_conf_own_cert()` fails | `m_open = false` | `ERR_IO` |
| `mbedtls_net_connect()` fails | `m_open = false`; `m_client_net[0].fd = -1` | `ERR_IO` |
| `mbedtls_net_set_block()` fails | `m_client_net[0]` freed + re-inited; `m_open = false` | `ERR_IO` |
| `mbedtls_ssl_setup()` fails | `m_client_net[0]` freed; `m_open = false` | `ERR_IO` |
| `mbedtls_ssl_handshake()` fails | `m_ssl[0]` freed + re-inited; `m_client_net[0]` freed + re-inited; `m_open = false` | `ERR_IO` |

In all error cases the `TlsTcpBackend` object is left in a safe state with `m_open = false`. The user may destroy the object or retry `init()` (provided the constructor has been called again; re-calling `init()` on the same object is guarded by the `!m_open` assertion — after a failure, since `m_open` is still false, re-init is technically possible but not explicitly supported).

---

## 10. External Interactions

| Call | Purpose | Library / OS |
|------|---------|--------------|
| `psa_crypto_init()` | Initialise PSA Crypto DRBG | mbedTLS 4.0 PSA |
| `mbedtls_ssl_config_defaults()` | STREAM / CLIENT / PRESET_DEFAULT | mbedTLS |
| `mbedtls_ssl_conf_authmode()` | Set peer verify mode | mbedTLS |
| `mbedtls_x509_crt_parse_file()` | Parse PEM cert from disk | mbedTLS + POSIX file I/O |
| `mbedtls_ssl_conf_ca_chain()` | Register CA for verification | mbedTLS |
| `mbedtls_pk_parse_keyfile()` | Parse PEM private key from disk | mbedTLS + POSIX file I/O |
| `mbedtls_ssl_conf_own_cert()` | Bind cert + key to ssl_conf | mbedTLS |
| `mbedtls_net_connect()` | DNS resolution + `socket()` + `connect()` | mbedTLS (wraps POSIX) |
| `mbedtls_net_set_block()` | `fcntl()` to ensure blocking mode | mbedTLS (wraps fcntl) |
| `mbedtls_ssl_setup()` | Associate ssl_conf with ssl_context | mbedTLS |
| `mbedtls_ssl_set_bio()` | Wire BIO callbacks to net_context | mbedTLS |
| `mbedtls_ssl_handshake()` | Full TLS client handshake | mbedTLS |
| `mbedtls_ssl_get_ciphersuite()` | Read negotiated cipher for logging | mbedTLS |
| `mbedtls_strerror()` | Error code to string | mbedTLS |

---

## 11. State Changes / Side Effects

| What changes | When | Before → After |
|---|---|---|
| `m_is_server` | `init()` entry | `false` → `false` (no change; confirmed) |
| `m_tls_enabled` | `init()` entry | `false` → `true` |
| `m_cfg` | `init()` entry | zeroed → copy of `config` |
| `m_ssl_conf` | `setup_tls_config()` | zero-initialised → configured for STREAM/CLIENT |
| `m_cert` | `setup_tls_config()` | empty → parsed certificate |
| `m_ca_cert` | `setup_tls_config()` (conditional) | empty → parsed CA cert |
| `m_pkey` | `setup_tls_config()` | empty → parsed private key |
| `m_client_net[0].fd` | `mbedtls_net_connect()` | -1 → valid connected fd |
| `m_ssl[0]` | `ssl_setup()` + `ssl_handshake()` | zero-initialised → fully established TLS session |
| `m_client_count` | `connect_to_server()` success | 0 → 1 |
| `m_open` | `connect_to_server()` success | `false` → `true` |

---

## 12. Sequence Diagram

```
User              TlsTcpBackend        mbedTLS / PSA Crypto      File I/O      Server peer
  |                     |                      |                     |                |
  |--- init(config) --->|                      |                     |                |
  |                     |--- psa_crypto_init() -->|                  |                |
  |                     |<-- PSA_SUCCESS ---------|                  |                |
  |                     |--- ssl_config_defaults() -->|              |                |
  |                     |--- ssl_conf_authmode() -->|                |                |
  |                     |         x509_crt_parse_file(CA) ---------->|               |
  |                     |         x509_crt_parse_file(cert) -------->|               |
  |                     |         pk_parse_keyfile() ---------------->|               |
  |                     |--- ssl_conf_own_cert() -->|                |                |
  |                     |--- net_connect(peer_ip:port) ------------------------------------------------->|
  |                     |<-- 0 (TCP connection established) ---------------------------------<|
  |                     |--- net_set_block() ---->|                  |                |
  |                     |--- ssl_setup() -------->|                  |                |
  |                     |--- ssl_set_bio() ------>|                  |                |
  |                     |--- ssl_handshake() ----->|                 |                |
  |                     |       [ClientHello] ------------------------------------------------->|
  |                     |       [ServerHello + Cert + HelloDone] <--------------------------<|
  |                     |       [ClientKeyExchange + CCS + Finished] ----------------------->|
  |                     |       [Server CCS + Finished] <-----------------------------------<|
  |                     |<-- 0 (handshake done) --|                  |                |
  |                     | (m_client_count=1, m_open=true)            |                |
  |<-- Result::OK -------|                        |                  |                |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (init(), called once):**
- The entire TLS handshake is performed during `init()`. By the time `init()` returns `OK`, the TLS tunnel is live.
- File I/O for cert/key material occurs during `init()`.
- `m_open` is set to `true` and `m_client_count` is set to 1 only after the handshake succeeds.
- The impairment engine is initialised but is not exercised until the first `send_message()` / `receive_message()` call.

**Runtime (after init() returns OK):**
- `send_message()` serializes the envelope, checks MTU against `SOCKET_RECV_BUF_BYTES`, applies impairments, then calls `tls_send_frame()` → `mbedtls_ssl_write()`.
- `receive_message()` calls `poll_clients_once()` → `recv_from_client()` → `tls_recv_frame()` → `mbedtls_ssl_read()`.
- No handshake activity occurs at runtime; the established session handles encryption/decryption transparently.

---

## 14. Known Risks / Observations

- **Blocking `init()`:** The TLS handshake in `connect_to_server()` is blocking. If the server is unreachable, the call will block until the OS TCP connect timeout expires (typically 75–130 seconds on Linux/macOS). There is no application-configurable connect timeout wrapping `mbedtls_net_connect()`.
- **Single-connection client model:** `TlsTcpBackend` in client mode uses only `m_client_net[0]` and `m_ssl[0]`. If `send_message()` is called when the connection is lost (e.g., the server closes), `tls_send_frame()` returns false but `m_client_count` is not decremented — the caller has no automatic reconnect path. Reconnect requires calling `close()` and `init()` again.
- **No SNI / hostname verification:** `mbedtls_ssl_set_hostname()` is not called, so Server Name Indication (SNI) is not sent and hostname verification is not performed against the server's certificate CN/SAN. When `verify_peer == true`, only the certificate chain signature is verified against the CA, not the hostname. [ASSUMPTION] This is acceptable for the current embedded/testing use case but would be a defect in a general-purpose TLS client.
- **`connect_to_server()` assertion `m_client_net[0U].fd < 0`:** This asserts that slot 0 is unused before connecting. If `connect_to_server()` were somehow called twice, this assertion would fire. The `!m_open` assertion in `init()` provides the outer guard.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `config.tls.role == TlsRole::CLIENT` is set by the caller when `config.is_server == false`.
- [ASSUMPTION] `mbedtls_net_connect()` performs DNS resolution synchronously on the calling thread. If the peer_ip is a hostname requiring DNS, DNS latency adds to the blocking duration of `init()`.
- [ASSUMPTION] The TLS handshake succeeding implies successful peer certificate verification when `verify_peer == true`. No further application-level certificate inspection is performed.
- [ASSUMPTION] The PSA Crypto entropy pool is adequately seeded by the time `psa_crypto_init()` returns. On embedded targets with limited entropy sources this may not hold.
- [ASSUMPTION] After `mbedtls_ssl_handshake()` returns 0, the negotiated protocol version is TLS 1.2 or higher. No explicit version check is performed by the application code.
