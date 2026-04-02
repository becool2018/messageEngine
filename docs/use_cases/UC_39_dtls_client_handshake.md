# UC_39 — DTLS client: connect UDP socket to peer, complete DTLS handshake

**HL Group:** HL-19 — DTLS-Encrypted UDP Transport
**Actor:** User
**Requirement traceability:** REQ-6.4.1, REQ-6.4.3, REQ-6.3.4, REQ-4.1.1, REQ-6.4.5

---

## 1. Use Case Overview

- **Trigger:** User calls `DtlsUdpBackend::init(config)` with `config.is_server == false` and `config.tls.tls_enabled == true`. File: `src/platform/DtlsUdpBackend.cpp`.
- **Goal:** Bind a local UDP socket, connect it to the DTLS server's address, and complete the DTLS 1.2 handshake (including responding to the server's HelloVerifyRequest cookie challenge) before any application data is exchanged.
- **Success outcome:** `init()` returns `Result::OK`. `m_open == true`. `m_has_session == true`. `m_ssl` holds an active DTLS session.
- **Error outcomes:**
  - `Result::ERR_INVALID` — certificate/key load fails.
  - `Result::ERR_IO` — UDP socket creation or connect fails.
  - `Result::ERR_IO` — DTLS handshake fails (certificate verification, cookie mismatch, etc.).

---

## 2. Entry Points

```cpp
// src/platform/DtlsUdpBackend.cpp
Result DtlsUdpBackend::init(const TransportConfig& config) override;
```

---

## 3. End-to-End Control Flow

1. `NEVER_COMPILED_OUT_ASSERT(!m_open)`.
2. `m_cfg = config`. `m_tls_enabled = config.tls.tls_enabled`. `m_recv_queue.init()`.
3. `psa_crypto_init()`.
4. **`setup_dtls_config(config.tls)`** — client-mode config:
   a. `mbedtls_ssl_config_defaults(&m_ssl_conf, CLIENT, TRANSPORT_DATAGRAM, PRESET_DEFAULT)`.
   b. `mbedtls_ssl_conf_rng()` — PSA RNG callbacks.
   c. **`load_tls_certs(tls_cfg)`** — loads cert/key/CA.
   d. No cookie context on client side — cookie is handled server-side.
5. **`connect_dtls_socket(peer_ip, peer_port)`** — UDP connect:
   a. `::socket(AF_INET, SOCK_DGRAM, 0)` — creates UDP socket; store as `m_udp_fd`.
   b. `::connect(m_udp_fd, peer_addr, ...)` — binds the socket to the peer address (POSIX UDP "connect" for datagrams).
   c. If connect fails: log `WARNING_LO`; return `ERR_IO`.
6. **`dtls_client_handshake()`** — TLS setup and handshake:
   a. `mbedtls_ssl_setup(&m_ssl, &m_ssl_conf)`.
   b. If `verify_peer && peer_hostname[0] != '\0'`: `mbedtls_ssl_set_hostname(&m_ssl, peer_hostname)`.
   c. `mbedtls_ssl_set_bio(&m_ssl, &m_udp_fd, ...)`.
   d. `mbedtls_ssl_set_timer_cb(&m_ssl, ...)` — DTLS retransmission timer.
   e. **Handshake loop** (bounded by `MBEDTLS_HANDSHAKE_RETRY`):
      - `mbedtls_ssl_handshake(&m_ssl)` — includes ClientHello, HelloVerifyRequest, ClientHello+cookie, ServerHello, Finished.
      - Retry on `WANT_READ`/`WANT_WRITE`.
      - On success (0): proceed.
      - On fatal: log `WARNING_LO`; return `ERR_IO`.
7. `m_is_server = false; m_has_session = true; m_open = true`.
8. Returns `Result::OK`.

---

## 4. Call Tree

```
DtlsUdpBackend::init(config)                        [DtlsUdpBackend.cpp]
 ├── psa_crypto_init()
 ├── DtlsUdpBackend::setup_dtls_config()
 │    ├── mbedtls_ssl_config_defaults()             [DTLS CLIENT mode]
 │    ├── mbedtls_ssl_conf_rng()
 │    └── DtlsUdpBackend::load_tls_certs()
 ├── DtlsUdpBackend::connect_dtls_socket()
 │    ├── ::socket(AF_INET, SOCK_DGRAM, 0)
 │    └── ::connect(m_udp_fd, peer_addr)
 └── DtlsUdpBackend::dtls_client_handshake()
      ├── mbedtls_ssl_setup()
      ├── mbedtls_ssl_set_hostname()                [if verify_peer]
      ├── mbedtls_ssl_set_bio()
      ├── mbedtls_ssl_set_timer_cb()
      └── [handshake loop] mbedtls_ssl_handshake()
```

---

## 5. Key Components Involved

- **`setup_dtls_config()`** — `MBEDTLS_SSL_TRANSPORT_DATAGRAM` CLIENT mode with PSA RNG and certs (REQ-6.4.1).
- **`connect_dtls_socket()`** — POSIX `::connect()` on a UDP socket to "connect" it to the server address, enabling `send()`/`recv()` without `sendto()`/`recvfrom()`.
- **`dtls_client_handshake()`** — CC-reduction helper encapsulating ssl_setup, BIO, timer, and handshake loop.
- **`mbedtls_ssl_set_timer_cb()`** — DTLS retransmission timer; required for packet-loss resilience during handshake (REQ-6.4.3).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `tls_enabled == false` | Plaintext UDP; skip DTLS setup | DTLS setup proceeds |
| `verify_peer && hostname set` | `ssl_set_hostname()` called | No hostname verification |
| `::connect()` fails | Log; return ERR_IO | Proceed to handshake |
| Handshake returns WANT_READ/WRITE | Retry | Success (0) or fatal |
| Handshake fatal | Log; return ERR_IO | `m_has_session = true; m_open = true` |

---

## 7. Concurrency / Threading Behavior

- `init()` single-threaded.
- `dtls_client_handshake()` blocks in the handshake loop until complete or error.
- `m_ssl` is a single DTLS session context.
- No `std::atomic` operations in this flow.

---

## 8. Memory & Ownership Semantics

- `m_ssl`, `m_ssl_conf`, `m_cert`, `m_pkey`, `m_ca_cert` — statically allocated members.
- `m_udp_fd` — POSIX file descriptor; owned by `DtlsUdpBackend`; closed in `close()`.
- **Init-phase heap allocation:** mbedTLS allocates internally during cert parsing and handshake. Documented Power of 10 Rule 3 deviation (init-phase only).

---

## 9. Error Handling Flow

- **Cert/key load failure:** `load_tls_certs()` returns `ERR_INVALID`; `init()` fails early. `m_open` remains false.
- **UDP connect failure:** `m_udp_fd` closed; `m_open` remains false. Caller must retry.
- **DTLS handshake failure:** `mbedtls_ssl_free(&m_ssl)`; socket closed. `m_open` false.

---

## 10. External Interactions

- **POSIX sockets:** `::socket()`, `::connect()` — creates and connects UDP socket.
- **PSA Crypto:** `psa_crypto_init()` initializes entropy.
- **File I/O:** PEM cert/key/CA files read during `load_tls_certs()`.
- **mbedTLS DTLS:** Handshake messages exchanged with the DTLS server over UDP.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DtlsUdpBackend` | `m_ssl_conf` | uninitialized | configured DTLS CLIENT |
| `DtlsUdpBackend` | `m_udp_fd` | -1 | connected UDP socket fd |
| `DtlsUdpBackend` | `m_ssl` | uninitialized | active DTLS session |
| `DtlsUdpBackend` | `m_has_session` | false | true |
| `DtlsUdpBackend` | `m_open` | false | true |

---

## 12. Sequence Diagram

```
User -> DtlsUdpBackend::init(config)  [client, tls_enabled=true]
  -> psa_crypto_init()
  -> setup_dtls_config()              [CLIENT DATAGRAM mode, load certs]
  -> connect_dtls_socket(peer_ip, port)
       -> ::socket(AF_INET, SOCK_DGRAM, 0)
       -> ::connect(fd, peer_addr)
  -> dtls_client_handshake()
       -> mbedtls_ssl_setup()
       -> mbedtls_ssl_set_hostname()  [if verify_peer]
       -> mbedtls_ssl_set_bio()
       -> mbedtls_ssl_set_timer_cb()
       -> mbedtls_ssl_handshake()     [ClientHello -> HelloVerifyRequest -> ClientHello+cookie -> Finished]
       <- 0 (success)
  <- Result::OK  [m_has_session=true; m_open=true]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- DTLS server must be listening and have a matching certificate.
- PEM files must be readable at configured paths.

**Runtime:**
- After successful `init()`, all `send_message()` / `receive_message()` calls use `mbedtls_ssl_write()` / `mbedtls_ssl_read()` on `m_ssl`.

---

## 14. Known Risks / Observations

- **Cookie challenge:** Client must handle `MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED` by retrying the handshake from scratch. The handshake loop retry covers this case.
- **Timer-driven retransmission:** Without the timer callback, packet loss during handshake on a lossy network would cause indefinite blocking (REQ-6.4.3).
- **Hostname verification:** If `verify_peer == true` but `peer_hostname` is empty, `ssl_set_hostname()` is not called, which may cause certificate CN mismatch.
- **UDP "connect" semantics:** `::connect()` on a UDP socket filters received datagrams by source address; datagrams from other addresses are silently dropped by the OS.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `dtls_client_handshake()` is a CC-reduction helper factored out of `init()` for the client DTLS setup and handshake steps. Exact function name inferred from DtlsUdpBackend.cpp structure.
- `[ASSUMPTION]` `MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED` is handled by the handshake loop retry mechanism, not as a fatal error.
- `[ASSUMPTION]` The DTLS timer callbacks use the same structure as the server-side callbacks.
