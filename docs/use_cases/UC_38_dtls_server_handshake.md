# UC_38 — DTLS server: bind, receive ClientHello, perform cookie exchange, complete DTLS handshake

**HL Group:** HL-19 — DTLS-Encrypted UDP Transport
**Actor:** User
**Requirement traceability:** REQ-6.4.1, REQ-6.4.2, REQ-6.3.4, REQ-4.1.1, REQ-6.4.5

---

## 1. Use Case Overview

- **Trigger:** User calls `DtlsUdpBackend::init(config)` with `config.is_server == true` and `config.tls.tls_enabled == true`. File: `src/platform/DtlsUdpBackend.cpp`.
- **Goal:** Bind a UDP socket, await a DTLS ClientHello, perform the cookie exchange to prevent amplification attacks, then complete the DTLS 1.2 handshake with the client.
- **Success outcome:** `init()` returns `Result::OK`. `m_open == true`. The subsequent `accept_dtls_client()` (called from `receive_message()`) completes the DTLS handshake and installs an active SSL session.
- **Error outcomes:**
  - `Result::ERR_INVALID` — certificate/key load fails.
  - `Result::ERR_IO` — socket bind fails.
  - Per-handshake failure: logged `WARNING_LO`; that client session is discarded.

---

## 2. Entry Points

```cpp
// src/platform/DtlsUdpBackend.cpp
Result DtlsUdpBackend::init(const TransportConfig& config) override;
```

---

## 3. End-to-End Control Flow

**During `init()`:**
1. `NEVER_COMPILED_OUT_ASSERT(!m_open)`.
2. `m_cfg = config`. `m_tls_enabled = config.tls.tls_enabled`. `m_recv_queue.init()`.
3. `psa_crypto_init()` — initializes PSA Crypto entropy source.
4. **`setup_dtls_config(config.tls)`** — configures `mbedtls_ssl_config`:
   a. `mbedtls_ssl_config_defaults(&m_ssl_conf, SERVER, TRANSPORT_DATAGRAM, PRESET_DEFAULT)`.
   b. `mbedtls_ssl_conf_rng()` — PSA RNG callbacks.
   c. **`load_tls_certs(tls_cfg)`** — loads cert/key/CA (same as TlsTcpBackend).
   d. **`mbedtls_ssl_cookie_setup(&m_cookie_ctx, ...)`** — arms the DTLS cookie exchange context.
   e. `mbedtls_ssl_conf_dtls_cookies(&m_ssl_conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &m_cookie_ctx)` — registers cookie callbacks.
5. **`bind_udp_socket(ip, port)`** — creates UDP socket, sets `SO_REUSEADDR`, binds.
6. `m_is_server = true; m_open = true`.
7. Returns `Result::OK`.

**During `accept_dtls_client()` (called from `receive_message()`):**
1. `mbedtls_net_init(&m_client_net)` — reset client net context.
2. `mbedtls_ssl_setup(&m_ssl, &m_ssl_conf)`.
3. `mbedtls_ssl_set_bio(&m_ssl, &m_udp_fd, ...)` — wire SSL to UDP socket.
4. `mbedtls_ssl_set_timer_cb(&m_ssl, ...)` — set DTLS retransmission timer callbacks.
5. **DTLS handshake loop** (bounded by `MBEDTLS_HANDSHAKE_RETRY`):
   - First ClientHello: cookie exchange — mbedTLS sends HelloVerifyRequest; client resends with cookie.
   - `mbedtls_ssl_handshake(&m_ssl)` — retry on `WANT_READ`/`WANT_WRITE`.
   - On success (0): client accepted.
   - On fatal: log `WARNING_LO`; `mbedtls_ssl_free(&m_ssl)`. Skip client.
6. `m_has_session = true`.

---

## 4. Call Tree

```
DtlsUdpBackend::init(config)                      [DtlsUdpBackend.cpp]
 ├── psa_crypto_init()
 ├── DtlsUdpBackend::setup_dtls_config()
 │    ├── mbedtls_ssl_config_defaults()           [DTLS SERVER mode]
 │    ├── mbedtls_ssl_conf_rng()
 │    ├── DtlsUdpBackend::load_tls_certs()
 │    ├── mbedtls_ssl_cookie_setup()
 │    └── mbedtls_ssl_conf_dtls_cookies()
 └── DtlsUdpBackend::bind_udp_socket()

DtlsUdpBackend::accept_dtls_client()              [DtlsUdpBackend.cpp]
 ├── mbedtls_ssl_setup()
 ├── mbedtls_ssl_set_bio()
 ├── mbedtls_ssl_set_timer_cb()
 └── [handshake loop] mbedtls_ssl_handshake()     [cookie exchange + DTLS handshake]
```

---

## 5. Key Components Involved

- **`setup_dtls_config()`** — configures `MBEDTLS_SSL_TRANSPORT_DATAGRAM` mode with PSA RNG, certs, and cookie setup (REQ-6.4.2).
- **`mbedtls_ssl_cookie_setup/write/check`** — DTLS cookie exchange callbacks bound to client `(address, port)` identity to prevent amplification (REQ-6.4.2).
- **`mbedtls_ssl_set_timer_cb()`** — DTLS retransmission timer; drives retransmission without busy-waiting (REQ-6.4.3).
- **`accept_dtls_client()`** — per-client: arm SSL context, run handshake loop with cookie exchange.
- **mbedTLS 4.0 PSA backend** — DTLS record-layer security over UDP.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `tls_enabled == false` | Plaintext UDP path; skip DTLS setup | DTLS setup proceeds |
| `verify_peer == true` | Load CA cert; `AUTHMODE_REQUIRED` | Skip CA; `AUTHMODE_NONE` |
| Cookie exchange required | HelloVerifyRequest sent; retry | First ClientHello not repeated |
| `mbedtls_ssl_handshake()` returns WANT_READ/WRITE | Retry | Success (0) or fatal |
| Handshake fatal | Log WARNING_LO; free SSL; skip | `m_has_session = true` |

---

## 7. Concurrency / Threading Behavior

- `init()` single-threaded.
- `accept_dtls_client()` called from the application receive thread during `receive_message()`.
- `m_ssl` is a single DTLS session context; only one client session at a time.
- No `std::atomic` operations in this flow.

---

## 8. Memory & Ownership Semantics

- `m_ssl_conf`, `m_cookie_ctx`, `m_cert`, `m_ca_cert`, `m_pkey` — statically allocated as `DtlsUdpBackend` members.
- `m_ssl` — single mbedTLS DTLS context; statically allocated member.
- **Init-phase heap allocation:** mbedTLS allocates internally during cert parsing and handshake setup. Documented Power of 10 Rule 3 deviation (init-phase only).

---

## 9. Error Handling Flow

- **Cert/key load failure:** `load_tls_certs()` returns `ERR_INVALID`; `init()` fails. Transport not opened.
- **Socket bind failure:** `bind_udp_socket()` returns `ERR_IO`; `init()` fails. `m_open` remains false.
- **Handshake failure per attempt:** `mbedtls_ssl_free(&m_ssl)`; session context cleared. Next `receive_message()` call will retry accept.
- **Cookie exchange:** Not a failure — client must resend ClientHello with cookie; loop retries automatically.

---

## 10. External Interactions

- **POSIX sockets:** UDP socket created and bound via `::socket()`, `::bind()`.
- **PSA Crypto:** `psa_crypto_init()` initializes entropy.
- **File I/O:** PEM cert/key/CA files read during `load_tls_certs()`.
- **mbedTLS DTLS:** Cookie exchange and DTLS handshake messages exchanged over UDP socket.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DtlsUdpBackend` | `m_ssl_conf` | uninitialized | configured DTLS SERVER with cookie |
| `DtlsUdpBackend` | `m_cookie_ctx` | uninitialized | armed cookie context |
| `DtlsUdpBackend` | `m_open` | false | true |
| `DtlsUdpBackend` | `m_ssl` | uninitialized | active DTLS session |
| `DtlsUdpBackend` | `m_has_session` | false | true |

---

## 12. Sequence Diagram

```
User -> DtlsUdpBackend::init(config)   [server, tls_enabled=true]
  -> psa_crypto_init()
  -> setup_dtls_config()
       -> load_tls_certs()
       -> mbedtls_ssl_cookie_setup()
       -> mbedtls_ssl_conf_dtls_cookies()
  -> bind_udp_socket(ip, port)
  <- Result::OK

[Client sends ClientHello]
User -> DtlsUdpBackend::receive_message()
  -> accept_dtls_client()
       -> mbedtls_ssl_setup()
       -> mbedtls_ssl_set_bio()
       -> mbedtls_ssl_set_timer_cb()
       -> mbedtls_ssl_handshake()       [HelloVerifyRequest -> ClientHello+cookie -> ServerHello -> Finished]
       <- 0 (success)
  [m_has_session = true]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- PEM cert, key, and (optionally) CA files exist at configured paths.
- `psa_crypto_init()` succeeds.

**Runtime:**
- After `init()`, each client connection triggers `accept_dtls_client()` (one session at a time).
- After successful handshake, all `send_message()` / `receive_message()` calls use `mbedtls_ssl_write()` / `mbedtls_ssl_read()` on `m_ssl`.

---

## 14. Known Risks / Observations

- **Single-session limitation:** `DtlsUdpBackend` supports one DTLS session at a time (unlike `TlsTcpBackend`'s 8-slot array). A second client cannot connect until the session is closed.
- **Cookie exchange amplification protection:** Cookie exchange is mandatory server-side (REQ-6.4.2). The cookie is bound to the client's `(address, port)`; a spoofed source address will fail the cookie check.
- **Timer-driven retransmission:** Without `mbedtls_ssl_set_timer_cb()`, DTLS packet loss during handshake would block indefinitely. The timer ensures retransmission (REQ-6.4.3).
- **Init-phase heap allocation:** mbedTLS heap use during cert parsing is documented; not on the send/receive critical path.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `accept_dtls_client()` is called from inside `receive_message()` when `!m_has_session`. The exact triggering condition is inferred from DtlsUdpBackend.cpp structure.
- `[ASSUMPTION]` `MBEDTLS_HANDSHAKE_RETRY` is the bounded retry constant for the handshake loop; exact value inferred from the pattern used in TlsTcpBackend.
- `[ASSUMPTION]` The DTLS timer callbacks use a simple monotonic deadline structure; exact implementation inferred from mbedTLS 4.0 usage pattern.
