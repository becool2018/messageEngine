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
      - **Handshake loop** (bounded by retry count):
        - `mbedtls_ssl_handshake(&m_ssl[0])` — retry on `WANT_READ`/`WANT_WRITE`.
        - On success (0): proceed.
        - On fatal: log `WARNING_LO`; return `ERR_IO`.
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
           ├── mbedtls_ssl_set_bio()
           └── [handshake loop] mbedtls_ssl_handshake()
```

---

## 5. Key Components Involved

- **`tls_connect_handshake()`** — CC-reduction helper extracted from `connect_to_server()`. Encapsulates ssl_setup, hostname, BIO, and handshake steps.
- **`mbedtls_ssl_set_hostname()`** — Sets SNI/certificate hostname for peer verification. Required when `verify_peer == true`.
- **mbedTLS 4.0 client mode** — Performs ClientHello → ServerHello → Certificate → Finished TLS handshake.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `tls_enabled == false` | Plaintext TCP; skip handshake | Perform TLS handshake |
| `verify_peer && hostname set` | `ssl_set_hostname()` called | No hostname verification |
| Handshake returns WANT_READ/WRITE | Retry | Success (0) or fatal |
| Handshake fatal | Log; return ERR_IO | `m_open = true` |

---

## 7. Concurrency / Threading Behavior

- `init()` single-threaded.
- `tls_connect_handshake()` blocks in the handshake loop until complete or error.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `m_ssl[0]` — mbedTLS TLS context for the one client connection. Statically allocated as array member.
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
            -> mbedtls_ssl_set_bio()
            -> mbedtls_ssl_handshake()    [TLS exchange with server]
            <- 0 (success)
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
- **No renegotiation support:** TLS 1.3 disables renegotiation; session tickets or connection reuse are not implemented.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `tls_connect_handshake()` is a CC-reduction helper extracted from `connect_to_server()`. It encapsulates the mbedTLS setup + handshake steps for the client socket at slot 0.
