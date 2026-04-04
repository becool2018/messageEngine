# UC_37 — TLS client: connect to peer and complete TLS handshake before first message

**HL Group:** HL-18 — TLS-Encrypted TCP Transport
**Actor:** User
**Requirement traceability:** REQ-6.3.4, REQ-4.1.1, REQ-6.1.1, REQ-6.1.2

---

## 1. Use Case Overview

- **Trigger:** User calls `TlsTcpBackend::init(config)` with `config.is_server == false` and `config.tls.tls_enabled == true`. File: `src/platform/TlsTcpBackend.cpp`.
- **Goal:** Establish a TCP connection to the configured peer, then complete a TLS 1.2+ handshake before any application data is sent or received.
- **Success outcome:** `init()` returns `Result::OK`. `m_client_net[0]` and `m_ssl[0]` are active with a completed TLS session. `m_open == true`.
- **Error outcomes:**
  - `Result::ERR_IO` — TCP connect fails.
  - `Result::ERR_INVALID` — certificate/key load fails.
  - `Result::ERR_IO` — TLS handshake fails (certificate verification error, cipher mismatch, etc.).

---

## 2. Entry Points

```cpp
// src/platform/TlsTcpBackend.cpp
Result TlsTcpBackend::init(const TransportConfig& config) override;
```

---

## 3. End-to-End Control Flow

1. `NEVER_COMPILED_OUT_ASSERT(!m_open)`.
2. `m_cfg = config`. `m_tls_enabled = config.tls.tls_enabled`. `m_recv_queue.init()`.
3. `psa_crypto_init()`.
4. **`setup_tls_config(config.tls)`** — client-mode config:
   a. `mbedtls_ssl_config_defaults(&m_ssl_conf, CLIENT, TRANSPORT_STREAM, PRESET_DEFAULT)`.
   b. Sets PSA RNG callbacks.
   c. **`load_tls_certs(tls_cfg)`** — loads cert/key/CA as in server.
5. **`connect_to_server()`** is called (`TlsTcpBackend.cpp`):
   a. `mbedtls_net_connect(&m_client_net[0], peer_ip, port_str, NET_PROTO_TCP)` — TCP connect with mbedTLS net context.
   b. If connect fails: log `WARNING_LO`; return `ERR_IO`.
   c. `m_client_count = 1`.
   d. If `tls_enabled`: **`tls_connect_handshake()`** is called:
      - `mbedtls_net_set_block(&m_client_net[0])`.
      - `mbedtls_ssl_setup(&m_ssl[0], &m_ssl_conf)`.
      - If `verify_peer && peer_hostname[0] != '\0'`: `mbedtls_ssl_set_hostname(&m_ssl[0], peer_hostname)`.
      - `mbedtls_ssl_set_bio(&m_ssl[0], &m_client_net[0], mbedtls_net_send, mbedtls_net_recv, nullptr)`.
      - If `session_resumption_enabled && m_session_saved`: **`try_load_client_session()`** — calls `mbedtls_ssl_set_session(&m_ssl[0], &m_saved_session)` to present a saved session ticket for abbreviated resumption. Non-fatal if `ssl_set_session()` fails; full handshake proceeds.
      - `mbedtls_ssl_set_bio(&m_ssl[0], &m_client_net[0], mbedtls_net_send, mbedtls_net_recv, nullptr)`.
      - **Handshake loop** (bounded by retry count):
        - `mbedtls_ssl_handshake(&m_ssl[0])` — retry on `WANT_READ`/`WANT_WRITE`.
        - On success (0): proceed.
        - On fatal: log `WARNING_LO`; return `ERR_IO`.
      - If `session_resumption_enabled` and handshake succeeded: **`try_save_client_session()`** — calls `mbedtls_ssl_get_session(&m_ssl[0], &m_saved_session)` to save the negotiated session for the next connection. Sets `m_session_saved = true` on success; non-fatal on failure.
6. `m_is_server = false; m_open = true`.
7. Returns `Result::OK`.

---

## 4. Call Tree

```
TlsTcpBackend::init(config)                      [TlsTcpBackend.cpp]
 ├── psa_crypto_init()
 ├── setup_tls_config()
 │    ├── mbedtls_ssl_config_defaults() [CLIENT mode]
 │    └── load_tls_certs()
 └── TlsTcpBackend::connect_to_server()
      ├── mbedtls_net_connect()
      └── TlsTcpBackend::tls_connect_handshake()
           ├── mbedtls_ssl_setup()
           ├── mbedtls_ssl_set_hostname()
           ├── [if resumption enabled & session saved] try_load_client_session()
           │    └── mbedtls_ssl_set_session()
           ├── mbedtls_ssl_set_bio()
           ├── [handshake loop] mbedtls_ssl_handshake()
           └── [if resumption enabled & handshake ok] try_save_client_session()
                └── mbedtls_ssl_get_session()
```

---

## 5. Key Components Involved

- **`tls_connect_handshake()`** — CC-reduction helper extracted from `connect_to_server()`. Encapsulates ssl_setup, hostname, session load, BIO, and handshake steps.
- **`try_load_client_session()`** — CC-reduction helper that calls `mbedtls_ssl_set_session()` to present a saved session ticket before the handshake. Non-fatal on failure. Guarded by `#if defined(MBEDTLS_SSL_SESSION_TICKETS)`.
- **`try_save_client_session()`** — CC-reduction helper that calls `mbedtls_ssl_get_session()` to save the negotiated session after a successful handshake. Non-fatal on failure. Sets `m_session_saved`.
- **`mbedtls_ssl_set_hostname()`** — Sets SNI/certificate hostname for peer verification. Required when `verify_peer == true`.
- **mbedTLS 4.0 client mode** — Performs ClientHello → ServerHello → Certificate → Finished TLS handshake; may perform abbreviated resumed handshake when a valid session ticket is presented.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `tls_enabled == false` | Plaintext TCP; skip handshake | Perform TLS handshake |
| `verify_peer && hostname set` | `ssl_set_hostname()` called | No hostname verification |
| `session_resumption_enabled && m_session_saved` | `try_load_client_session()` called before handshake | Full handshake without session hint |
| `ssl_set_session()` fails in load helper | Log WARNING_LO; full handshake proceeds | Session hint presented; abbreviated handshake attempted |
| Handshake returns WANT_READ/WRITE | Retry | Success (0) or fatal |
| Handshake fatal | Log; return ERR_IO | `m_open = true` |
| `session_resumption_enabled && handshake ok` | `try_save_client_session()` called; `m_session_saved = true` | Session not saved |

---

## 7. Concurrency / Threading Behavior

- `init()` single-threaded.
- `tls_connect_handshake()` blocks in the handshake loop until complete or error.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `m_ssl[0]` — mbedTLS TLS context for the one client connection. Statically allocated as array member.
- `m_saved_session` — fixed-size `mbedtls_ssl_session` struct allocated as a `TlsTcpBackend` member. Holds the negotiated TLS session for resumption on next connect. No dynamic allocation (Power of 10 Rule 3).
- `m_session_saved` — bool flag; true when `m_saved_session` contains a valid resumable session.
- **Init-phase heap allocation:** mbedTLS allocates internally during cert parsing and handshake.

---

## 9. Error Handling Flow

- **TCP connect failure:** `m_client_count` stays 0; `m_open` remains false. Caller must retry.
- **TLS handshake failure:** `mbedtls_ssl_free(&m_ssl[0])`; close net context. `m_open` false.
- **Cert load failure:** `setup_tls_config()` returns `ERR_INVALID`; `init()` fails early.

---

## 10. External Interactions

- **File I/O:** Cert/key/CA PEM files read during `load_tls_certs()`.
- **POSIX sockets:** TCP connect via `mbedtls_net_connect()`.
- **PSA Crypto / mbedTLS:** Handshake messages exchanged with the TLS server.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TlsTcpBackend` | `m_ssl[0]` | uninitialized | active TLS session |
| `TlsTcpBackend` | `m_client_net[0]` | uninitialized | connected TCP net context |
| `TlsTcpBackend` | `m_client_count` | 0 | 1 |
| `TlsTcpBackend` | `m_open` | false | true |
| `TlsTcpBackend` | `m_saved_session` | (empty) | populated with session state (if resumption enabled and handshake succeeded) |
| `TlsTcpBackend` | `m_session_saved` | false | true (if session saved successfully) |

---

## 12. Sequence Diagram

```
User -> TlsTcpBackend::init(config)  [client, tls_enabled=true]
  -> psa_crypto_init()
  -> setup_tls_config()              [CLIENT mode, load certs]
  -> connect_to_server()
       -> mbedtls_net_connect()      [TCP connection]
       -> tls_connect_handshake()
            -> mbedtls_ssl_setup()
            -> mbedtls_ssl_set_hostname()  [if verify_peer]
            -> try_load_client_session()   [if session_resumption_enabled && m_session_saved]
                 -> mbedtls_ssl_set_session()   [presents saved session ticket]
            -> mbedtls_ssl_set_bio()
            -> mbedtls_ssl_handshake()    [TLS exchange; may be abbreviated if ticket accepted]
            <- 0 (success)
            -> try_save_client_session()  [if session_resumption_enabled]
                 -> mbedtls_ssl_get_session()   [saves new session; m_session_saved=true]
  <- Result::OK [m_open=true; TLS session active]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- TLS server must be listening and have a matching certificate.
- PEM files must be readable.

**Runtime:**
- After successful `init()`, all `send_message()` / `receive_message()` calls use `tls_send_frame()` / `tls_recv_frame()` which go through `mbedtls_ssl_write()` / `mbedtls_ssl_read()`.

---

## 14. Known Risks / Observations

- **Hostname verification:** If `verify_peer == true` but `peer_hostname` is empty, `ssl_set_hostname()` is not called, which may cause certificate CN mismatch during handshake.
- **Blocking handshake:** The handshake loop blocks the caller until complete. Long network latency can extend `init()` duration.
- **Session resumption (REQ-6.3.4):** When `session_resumption_enabled == true`, the client saves the negotiated TLS session after each successful handshake (`try_save_client_session()`) and presents it on the next connect (`try_load_client_session()`). Both operations are non-fatal; failure falls back to a full handshake. Session state is stored in fixed-size `m_saved_session` (no dynamic allocation). Guarded by `#if defined(MBEDTLS_SSL_SESSION_TICKETS)` for build portability.
- **Single-lifecycle design:** `TlsTcpBackend` is initialized once; calling `init()` a second time on the same instance (after `close()`) is not supported. Each logical reconnect should use a new instance.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `tls_connect_handshake()` is a CC-reduction helper extracted from `connect_to_server()`. It encapsulates the mbedTLS setup + handshake steps for the client socket at slot 0.
- `[ASSUMPTION]` `try_load_client_session()` and `try_save_client_session()` are CC-reduction helpers extracted from `tls_connect_handshake()` to keep its cyclomatic complexity ≤ 10.
