# UC_39 — DTLS client: connect UDP socket to peer, complete DTLS handshake

**HL Group:** HL-19 — DTLS-Encrypted UDP Transport
**Actor:** User
**Requirement traceability:** REQ-4.1.1, REQ-6.3.4, REQ-6.4.1, REQ-6.4.3, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** User calls `DtlsUdpBackend::init(config)` where `config.is_server == false` and `config.tls.tls_enabled == true`. File: `src/platform/DtlsUdpBackend.cpp`.

**Goal:** Connect a DTLS client to a peer server: create a UDP socket, bind it locally, configure DTLS using PSA Crypto and loaded certificates, `connect()` the socket to the peer address, then complete the DTLS handshake.

**Success outcome:** `init()` returns `Result::OK`; `m_open == true`, `m_sock_fd >= 0`, DTLS session established. The client can now encrypt/decrypt messages via `m_ssl`.

**Error outcomes:**
- `Result::ERR_IO` — UDP socket/bind/connect fails; PSA Crypto init fails; cert/key loading fails; `inet_pton()` fails for peer IP; DTLS handshake fatally fails or exceeds `DTLS_HANDSHAKE_MAX_ITER`.

---

## 2. Entry Points

```cpp
Result DtlsUdpBackend::init(const TransportConfig& config)
    // src/platform/DtlsUdpBackend.cpp:541
```

`init()` calls `setup_dtls_config()` then `client_connect_and_handshake()`.

---

## 3. End-to-End Control Flow

1. `DtlsUdpBackend::init(config)` called with `config.kind == DTLS_UDP`, `config.is_server == false`, `config.tls.tls_enabled == true`.
2. Assertions: `config.kind == TransportKind::DTLS_UDP`, `!m_open`.
3. `m_cfg = config`; `m_is_server = false`; `m_tls_enabled = true`.
4. `m_recv_queue.init()` + `m_impairment.init(imp_cfg)`.
5. `m_sock_fd = m_sock_ops->create_udp()` — POSIX `socket(AF_INET, SOCK_DGRAM, 0)`. Returns `-1` on failure.
6. `m_sock_ops->set_reuseaddr(m_sock_fd)`. Failure: close fd, return `ERR_IO`.
7. `m_sock_ops->do_bind(m_sock_fd, config.bind_ip, config.bind_port)` — binds to local address. Failure: log FATAL, close fd, return `ERR_IO`.
8. `setup_dtls_config(config.tls)` — same as UC_38 steps 9a–9k, except:
   - `endpoint = MBEDTLS_SSL_IS_CLIENT`
   - Cookie setup block (`ssl_cookie_setup`) is **not** executed (server-only).
   Returns `ERR_IO` on any failure; fd is closed before returning.
9. Calls `client_connect_and_handshake()`.

   **Inside `client_connect_and_handshake()`:**
   a. Asserts `!m_is_server`, `m_sock_fd >= 0`.
   b. `memset(&peer, 0, sizeof(peer)); peer.sin_family = AF_INET; peer.sin_port = htons(m_cfg.peer_port)`.
   c. `m_ops->inet_pton_ipv4(m_cfg.peer_ip, &peer.sin_addr)` — converts IP string to binary. Returns `!= 1` on failure → log WARNING_HI, return `ERR_IO`.
   d. `m_ops->net_connect(m_sock_fd, (const sockaddr*)&peer, sizeof(peer))` — POSIX `connect()` on the UDP socket; subsequent `send()`/`recv()` are peer-locked. Returns `< 0` on failure → log WARNING_HI, return `ERR_IO`.
   e. `m_net_ctx.fd = m_sock_fd`.
   f. `m_ops->ssl_setup(&m_ssl, &m_ssl_conf)` — initialises DTLS session. Failure → return `ERR_IO`.
   g. `mbedtls_ssl_set_bio(&m_ssl, &m_net_ctx, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout)` — wires BIO (Rule 9 deviation: library symbols only).
   h. `mbedtls_ssl_set_timer_cb(&m_ssl, &m_timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay)` — wires retransmission timer (REQ-6.4.3).
   i. `mbedtls_ssl_set_mtu(&m_ssl, DTLS_MAX_DATAGRAM_BYTES)` — caps record to 1400 bytes (REQ-6.4.4).
   j. `run_dtls_handshake(&peer, sizeof(peer))` — bounded handshake loop (up to `DTLS_HANDSHAKE_MAX_ITER = 32`). Client does not encounter `HELLO_VERIFY_REQUIRED` (that is server behavior); typical returns are `WANT_READ`/`WANT_WRITE` during flight exchanges, then `0` on completion. Non-zero fatal error → free `m_ssl`, re-init, return `ERR_IO`.
   k. `m_open = true`.
   l. Logs INFO "DTLS handshake complete (client): cipher=...". Asserts `m_open`. Returns `Result::OK`.

10. If `client_connect_and_handshake()` fails: close fd, `m_sock_fd = -1`, return error.
11. `init()` asserts `m_open`. Returns `Result::OK`.

---

## 4. Call Tree

```
DtlsUdpBackend::init()
 ├── m_recv_queue.init()
 ├── m_impairment.init()
 ├── m_sock_ops->create_udp()
 ├── m_sock_ops->set_reuseaddr()
 ├── m_sock_ops->do_bind()
 ├── setup_dtls_config()              [CLIENT endpoint, no cookie setup]
 │    ├── m_ops->crypto_init()
 │    ├── m_ops->ssl_config_defaults()  [DATAGRAM]
 │    ├── mbedtls_ssl_conf_handshake_timeout()
 │    ├── mbedtls_ssl_conf_read_timeout()
 │    ├── m_ops->x509_crt_parse_file()  [conditional CA]
 │    ├── m_ops->x509_crt_parse_file()  [own cert]
 │    ├── m_ops->pk_parse_keyfile()
 │    └── m_ops->ssl_conf_own_cert()
 └── client_connect_and_handshake()
      ├── m_ops->inet_pton_ipv4()
      ├── m_ops->net_connect()          [POSIX connect()]
      ├── m_ops->ssl_setup()
      ├── mbedtls_ssl_set_bio()
      ├── mbedtls_ssl_set_timer_cb()
      ├── mbedtls_ssl_set_mtu()
      └── run_dtls_handshake()
           └── [loop] m_ops->ssl_handshake()
                       ├── WANT_READ/WANT_WRITE → continue
                       └── 0 → done
```

---

## 5. Key Components Involved

- **`DtlsUdpBackend`**: Owns all DTLS state. In client mode `m_is_server == false`; no `m_cookie_ctx` usage.
- **`setup_dtls_config()`**: Same shared setup as server but with `MBEDTLS_SSL_IS_CLIENT` and no cookie callback registration.
- **`client_connect_and_handshake()`**: Converts peer IP to binary, locks socket to peer via `connect()`, wires DTLS session, then runs the handshake loop.
- **`run_dtls_handshake()`**: Same bounded loop as UC_38 but from the client's perspective: no `HELLO_VERIFY_REQUIRED` handling needed (the client will receive a HelloVerifyRequest from the server and handle it transparently within mbedTLS).
- **`IMbedtlsOps` / `ISocketOps`**: Injectable abstractions enabling unit tests to inject failure on any I/O step.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|---|---|---|
| `create_udp()` returns `< 0` | Log FATAL, return `ERR_IO` | Continue |
| `set_reuseaddr()` fails | Close fd, return `ERR_IO` | Continue |
| `do_bind()` fails | Log FATAL, close fd, return `ERR_IO` | Continue |
| `crypto_init() != PSA_SUCCESS` | Log FATAL, return `ERR_IO` | Continue |
| `inet_pton_ipv4() != 1` | Log WARNING_HI, return `ERR_IO` | Continue |
| `net_connect()` returns `< 0` | Log WARNING_HI, return `ERR_IO` | Continue |
| `ssl_setup()` fails | Return `ERR_IO` | Continue |
| `ssl_handshake()` == 0 | `done = true` | Continue loop |
| `ssl_handshake()` == `WANT_READ`/`WANT_WRITE` | Continue loop | — |
| `ssl_handshake()` other error | Log, return `ERR_IO` | — |
| Loop completes without `done` | Log, return `ERR_IO` | (success) |

---

## 7. Concurrency / Threading Behavior

- `init()` is fully synchronous; blocks until handshake completes or error occurs.
- No threads are created. All mbedTLS and POSIX calls are blocking on the caller thread.
- `mbedtls_timing_delay_context` tracks wall-clock time internally; thread-safe per mbedTLS documentation as long as only one thread calls into the SSL context at a time.
- After `init()` returns, `send_message()` and `receive_message()` must not be called concurrently without external locking (single-threaded usage model assumed).

---

## 8. Memory & Ownership Semantics

- **Stack buffers:**
  - `client_connect_and_handshake()`: `struct sockaddr_in peer` (16 bytes) on stack.
  - `log_mbedtls_err()`: `char err_buf[128]` on helper stack.
- **Fixed static members:** `m_wire_buf[8192]`, single `m_ssl`, `m_net_ctx`, `m_timer`.
- **Power of 10 Rule 3 deviation (init-phase):** `psa_crypto_init()`, cert parsing, `ssl_handshake()` heap-allocate inside mbedTLS. Init-phase only — not on critical send/receive path.
- **Ownership:** `DtlsUdpBackend` owns `m_sock_fd`; destructor calls `close()` which calls `m_sock_ops->do_close(m_sock_fd)`, plus all `mbedtls_xxx_free()` calls and `mbedtls_psa_crypto_free()`.

---

## 9. Error Handling Flow

| Error condition | Result code | State | Caller action |
|---|---|---|---|
| Socket/bind fails | `ERR_IO` | `m_sock_fd = -1`, `m_open = false` | Do not use backend |
| PSA/cert/key fails | `ERR_IO` | `m_sock_fd` closed | Same |
| `inet_pton` / `connect()` fails | `ERR_IO` | `m_sock_fd` closed | Same |
| DTLS handshake fails/times out | `ERR_IO` | `m_ssl` freed, `m_sock_fd` closed | Same |

All error paths close `m_sock_fd` and leave `m_open == false`.

---

## 10. External Interactions

- **POSIX:** `socket()`, `setsockopt(SO_REUSEADDR)`, `bind()`, `connect()` via `ISocketOps`.
- **mbedTLS:** `psa_crypto_init()`, `ssl_config_defaults(DATAGRAM)`, cert parse, `ssl_handshake()` via `IMbedtlsOps`.
- **File system:** cert/key PEM files read during `setup_dtls_config()`.

---

## 11. State Changes / Side Effects

| Member | Before `init()` | After success |
|---|---|---|
| `m_open` | `false` | `true` |
| `m_is_server` | `false` | `false` |
| `m_tls_enabled` | `false` | `true` |
| `m_sock_fd` | `-1` | valid UDP fd (bound, connect()ed to server) |
| `m_net_ctx.fd` | unset | `== m_sock_fd` |
| `m_ssl` | uninitialised | DTLS session established |

---

## 12. Sequence Diagram

```
User              DtlsUdpBackend      ISocketOps     IMbedtlsOps/PSA     Server
 |                     |                  |                |                |
 | init(config)        |                  |                |                |
 |-------------------> |                  |                |                |
 |                     | create_udp()     |                |                |
 |                     |----------------> |                |                |
 |                     | set_reuseaddr()  |                |                |
 |                     |----------------> |                |                |
 |                     | do_bind()        |                |                |
 |                     |----------------> |                |                |
 |                     | setup_dtls_config (CLIENT)        |                |
 |                     | crypto_init()                     |                |
 |                     |---------------------------------------->           |
 |                     | ssl_config_defaults(DATAGRAM)     |                |
 |                     |---------------------------------------->           |
 |                     | parse cert/key files              |                |
 |                     |---------------------------------------->           |
 |                     | inet_pton_ipv4() |                |                |
 |                     |---------------------------------------->           |
 |                     | connect(peer)    |                |                |
 |                     |----------------> |                |                |
 |                     | ssl_setup()                       |                |
 |                     |---------------------------------------->           |
 |                     | ssl_set_bio/timer/mtu             |                |
 |                     |---------------------------------------->           |
 |                     | ssl_handshake()  [WANT_READ loop] |                |
 |                     |                                   |  ClientHello   |
 |                     |                                                  ->|
 |                     |                                   |  HelloVerify   |
 |                     |                                                  <-|
 |                     |                                   |  ClientHello2  |
 |                     |                                                  ->|
 |                     |                                   |  [handshake]   |
 |                     |                                                  <->
 |                     | ssl_handshake() == 0              |                |
 | <-- Result::OK ----- |                  |                |                |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:** All DTLS setup (socket, bind, connect, cert load, handshake) occurs in `init()`. Client `init()` is non-blocking on the poll step — it connects directly without waiting.

**Runtime:** After `init()` completes, `send_message()` / `receive_message()` exchange encrypted DTLS records using the established `m_ssl` session via `mbedtls_ssl_write()`/`mbedtls_ssl_read()`.

---

## 14. Known Risks / Observations

- **Server must be listening before client `init()` is called.** Unlike TCP, there is no OS-level connection queue for UDP — the DTLS handshake will fail (or time out after retransmission) if the server is not already bound and waiting.
- **No certificate revocation check** (same as UC_36 / UC_38).
- **`DTLS_HANDSHAKE_MAX_ITER = 32`** limits retry tolerance on high-loss paths.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The client does not encounter `MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED` because cookie exchange is server-initiated. The client side of cookie exchange is handled transparently by mbedTLS without requiring application-level session reset.
- `[ASSUMPTION]` `IMbedtlsOps::inet_pton_ipv4()` wraps `inet_pton(AF_INET, ...)`. Only IPv4 is supported; IPv6 peer IPs would silently fail at this step.
