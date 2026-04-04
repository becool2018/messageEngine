# UC_36 — TLS server: bind, listen, accept, and complete TLS handshake with each client

**HL Group:** HL-18 — TLS-Encrypted TCP Transport
**Actor:** User
**Requirement traceability:** REQ-6.3.4, REQ-4.1.1, REQ-6.1.1, REQ-6.1.2

---

## 1. Use Case Overview

- **Trigger:** User calls `TlsTcpBackend::init(config)` with `config.is_server == true` and `config.tls.tls_enabled == true`. File: `src/platform/TlsTcpBackend.cpp`.
- **Goal:** Load TLS certificates and key, bind a listening socket, and perform a TLS 1.2+ handshake with each incoming client connection before any application data is exchanged.
- **Success outcome:** `init()` returns `Result::OK`. `m_open == true`. Each call to `accept_and_handshake()` (inside `poll_clients_once()`) completes a full TLS handshake and adds the client to `m_client_net[idx]` / `m_ssl[idx]`.
- **Error outcomes:**
  - `Result::ERR_IO` — socket bind/listen fails.
  - `Result::ERR_INVALID` — certificate/key file load fails.
  - TLS handshake failure per client: logged `WARNING_LO`; that client is not added.

---

## 2. Entry Points

```cpp
// src/platform/TlsTcpBackend.cpp
Result TlsTcpBackend::init(const TransportConfig& config) override;
```

---

## 3. End-to-End Control Flow

**During `init()`:**
1. `NEVER_COMPILED_OUT_ASSERT(!m_open)`.
2. `m_cfg = config`. `m_tls_enabled = config.tls.tls_enabled`.
3. `m_recv_queue.init()`.
4. `psa_crypto_init()` — initializes PSA Crypto backend (RNG source for TLS).
5. **`setup_tls_config(config.tls)`** — configures `mbedtls_ssl_config`:
   a. `mbedtls_ssl_config_defaults(&m_ssl_conf, SERVER, TRANSPORT_STREAM, PRESET_DEFAULT)`.
   b. `mbedtls_ssl_conf_rng()` — sets PSA RNG callbacks.
   c. **`load_tls_certs(tls_cfg)`** — loads cert/key/CA:
      - `mbedtls_x509_crt_parse_file(&m_cert, cert_file)`.
      - `mbedtls_pk_parse_keyfile(&m_pkey, key_file, nullptr, ...)`.
      - If `verify_peer`: `mbedtls_x509_crt_parse_file(&m_ca_cert, ca_file)`, `mbedtls_ssl_conf_ca_chain(...)`, `mbedtls_ssl_conf_authmode(REQUIRED)`.
      - `mbedtls_ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey)`.
   d. If `session_resumption_enabled`: `mbedtls_ssl_conf_session_tickets(&m_ssl_conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED)` — enables server-side session ticket support so clients may resume (RFC 5077). Guarded by `#if defined(MBEDTLS_SSL_SESSION_TICKETS)`.
   e. Returns `Result::OK`.
6. **`bind_and_listen(ip, port)`** — creates, configures, binds, and listens on `m_listen_net`.
7. `m_is_server = true; m_open = true`.

**During `accept_and_handshake()` (called from `poll_clients_once()`):**
1. `mbedtls_net_accept(&m_listen_net, &m_client_net[m_client_count], nullptr, 0, nullptr)` — accepts pending TCP connection.
2. `mbedtls_net_set_block(&m_client_net[m_client_count])`.
3. `mbedtls_ssl_setup(&m_ssl[m_client_count], &m_ssl_conf)`.
4. `mbedtls_ssl_set_bio(&m_ssl[...], &m_client_net[...], mbedtls_net_send, mbedtls_net_recv, nullptr)`.
5. **TLS handshake loop** (bounded `MBEDTLS_HANDSHAKE_RETRY` iterations):
   - `mbedtls_ssl_handshake(&m_ssl[...])` — may return `WANT_READ`/`WANT_WRITE` (non-blocking).
   - Retry until `0` (success) or fatal error.
6. If handshake fails: log `WARNING_LO`; `mbedtls_ssl_free(&m_ssl[idx])` / close client net; skip.
7. `m_client_count++`.

---

## 4. Call Tree

```
TlsTcpBackend::init(config)                     [TlsTcpBackend.cpp]
 ├── psa_crypto_init()                           [mbedTLS PSA]
 ├── TlsTcpBackend::setup_tls_config()
 │    ├── mbedtls_ssl_config_defaults()
 │    ├── mbedtls_ssl_conf_rng()
 │    └── TlsTcpBackend::load_tls_certs()
 │         ├── mbedtls_x509_crt_parse_file()
 │         ├── mbedtls_pk_parse_keyfile()
 │         └── mbedtls_ssl_conf_own_cert()
 └── TlsTcpBackend::bind_and_listen()            [binds m_listen_net]

TlsTcpBackend::accept_and_handshake()            [TlsTcpBackend.cpp]
 ├── mbedtls_net_accept()
 ├── mbedtls_ssl_setup()
 ├── mbedtls_ssl_set_bio()
 └── [handshake loop] mbedtls_ssl_handshake()
```

---

## 5. Key Components Involved

- **`setup_tls_config()`** — Configures the shared `mbedtls_ssl_config` with PSA RNG, cipher suites, and auth mode.
- **`load_tls_certs()`** — Loads PEM cert/key/CA from files. CC-reduction helper.
- **`accept_and_handshake()`** — Per-client: accept TCP connection + full TLS handshake.
- **mbedTLS 4.0** — PSA Crypto backend. Heap allocation occurs during handshake (Power of 10 Rule 3 init-phase deviation, documented in TlsTcpBackend.hpp).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `tls_enabled == false` | Plaintext TCP path (no mbedTLS) | TLS setup |
| `verify_peer == true` | Load CA cert; set AUTHMODE_REQUIRED | Skip CA; AUTHMODE_NONE |
| `session_resumption_enabled == true` | Enable server-side session tickets via `mbedtls_ssl_conf_session_tickets()` | Session ticket support off |
| `mbedtls_ssl_handshake()` returns WANT_READ/WRITE | Retry | Success (0) or fatal |
| Handshake fatal error | Log; remove client | Add to table |

---

## 7. Concurrency / Threading Behavior

- `init()` single-threaded. `accept_and_handshake()` called from the application's receive thread.
- mbedTLS contexts `m_ssl[idx]` are per-connection and not shared across threads.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `m_ssl_conf`, `m_cert`, `m_ca_cert`, `m_pkey` — fixed-size mbedTLS contexts, statically allocated as `TlsTcpBackend` members.
- `m_client_net[MAX_TCP_CONNECTIONS]`, `m_ssl[MAX_TCP_CONNECTIONS]` — fixed arrays for up to 8 client TLS sessions.
- **Session resumption:** When `session_resumption_enabled == true`, the server calls `mbedtls_ssl_conf_session_tickets()` during `setup_tls_config()` so that connecting clients may present a session ticket and skip the full handshake on reconnect. No extra server-side state is stored beyond the shared `m_ssl_conf`. Guarded by `#if defined(MBEDTLS_SSL_SESSION_TICKETS)` (Power of 10 Rule 3: no additional dynamic allocation).
- **Init-phase heap allocation:** mbedTLS allocates internally during `x509_crt_parse_file`, `pk_parse_keyfile`, and `ssl_handshake`. This is a documented Power of 10 Rule 3 deviation (init-phase only, not on send/receive critical path).

---

## 9. Error Handling Flow

- **Cert/key load failure:** `load_tls_certs()` returns `ERR_INVALID`; `init()` fails. Transport not opened.
- **Handshake failure per client:** Client removed; other clients unaffected.
- **Max connections:** Same as TCP backend — 8 simultaneous TLS sessions maximum.

---

## 10. External Interactions

- **File I/O:** `mbedtls_x509_crt_parse_file()` and `mbedtls_pk_parse_keyfile()` read PEM files from disk.
- **POSIX sockets:** `mbedtls_net_bind()`, `mbedtls_net_accept()`.
- **PSA Crypto:** `psa_crypto_init()` initializes the entropy source.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TlsTcpBackend` | `m_ssl_conf` | uninitialized | configured with cert/RNG |
| `TlsTcpBackend` | `m_cert`, `m_pkey` | uninitialized | loaded from PEM files |
| `TlsTcpBackend` | `m_open` | false | true |
| `TlsTcpBackend` | `m_ssl[m_client_count]` | uninitialized | TLS session active |
| `TlsTcpBackend` | `m_client_count` | 0 | 0..8 |

---

## 12. Sequence Diagram

```
User -> TlsTcpBackend::init(config)  [server, tls_enabled=true]
  -> psa_crypto_init()
  -> setup_tls_config()
       -> load_tls_certs()
            -> mbedtls_x509_crt_parse_file()   [cert.pem]
            -> mbedtls_pk_parse_keyfile()        [key.pem]
  -> bind_and_listen(ip, port)
  <- Result::OK

[Client connects]
User -> TlsTcpBackend::receive_message() -> poll_clients_once()
  -> accept_and_handshake()
       -> mbedtls_net_accept()
       -> mbedtls_ssl_setup() -> mbedtls_ssl_set_bio()
       -> mbedtls_ssl_handshake()  [TLS 1.2+ negotiation]
       [m_client_count++]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions (init):**
- PEM cert, key, and (optionally) CA files exist at the configured paths.
- `psa_crypto_init()` succeeds.

**Runtime (per connection):**
- One `accept_and_handshake()` call per connecting client. The handshake loop is bounded.

---

## 14. Known Risks / Observations

- **Init-phase heap allocation:** mbedTLS allocates heap during cert parsing and handshake. This is isolated to init and per-connection setup; not on the message send/receive critical path.
- **Certificate file dependency:** PEM files must exist and be readable at init time. Failure at this point prevents all TLS communication.
- **Handshake blocking:** The handshake loop may block until the client completes the TLS exchange. Long-running handshakes can delay other clients.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `MBEDTLS_HANDSHAKE_RETRY` is the bounded retry limit for the handshake loop inside `accept_and_handshake()`. The exact constant name is inferred from TlsTcpBackend.cpp; it is a Power of 10 Rule 2 compliance measure.
