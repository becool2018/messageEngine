# UC_38 — DTLS server: bind, receive ClientHello, perform cookie exchange, complete DTLS handshake

**HL Group:** HL-19 — DTLS-Encrypted UDP Transport
**Actor:** User
**Requirement traceability:** REQ-4.1.1, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** User calls `DtlsUdpBackend::init(config)` where `config.is_server == true` and `config.tls.tls_enabled == true`. File: `src/platform/DtlsUdpBackend.cpp`.

**Goal:** Bring a DTLS server endpoint online: create and bind a UDP socket, wait for the first client datagram, perform DTLS cookie anti-replay exchange, then complete the DTLS handshake — all before `init()` returns.

**Success outcome:** `init()` returns `Result::OK`; `m_open == true`, `m_sock_fd >= 0`, and the DTLS session (`m_ssl`) is fully established. The server can now send/receive encrypted datagrams to/from the single connected peer.

**Error outcomes:**
- `Result::ERR_IO` — UDP socket creation, `SO_REUSEADDR`, or `bind()` fails; PSA Crypto init fails; cert/key loading fails; `recvfrom(MSG_PEEK)` fails; `connect()` to client fails; `ssl_setup()` fails; DTLS handshake fatally fails.
- `Result::ERR_TIMEOUT` — `poll()` waiting for the first client datagram times out (configurable via `config.connect_timeout_ms`, default 30 s).

---

## 2. Entry Points

```cpp
Result DtlsUdpBackend::init(const TransportConfig& config)
    // src/platform/DtlsUdpBackend.cpp:541
```

`init()` is the sole entry point; it internally calls `setup_dtls_config()` and `server_wait_and_handshake()` before returning.

---

## 3. End-to-End Control Flow

1. `DtlsUdpBackend::init(config)` called with `config.kind == DTLS_UDP`, `config.is_server == true`, `config.tls.tls_enabled == true`.
2. Assertions: `config.kind == TransportKind::DTLS_UDP`, `!m_open`.
3. `m_cfg = config`; `m_is_server = true`; `m_tls_enabled = true`.
4. `m_recv_queue.init()` — reset RingBuffer.
5. `impairment_config_default(imp_cfg)`; if `config.num_channels > 0`: `imp_cfg = config.channels[0].impairment`. Then `m_impairment.init(imp_cfg)`.
6. `m_sock_fd = m_sock_ops->create_udp()` — `socket(AF_INET, SOCK_DGRAM, 0)`. Returns `-1` on failure → log FATAL, return `ERR_IO`.
7. `m_sock_ops->set_reuseaddr(m_sock_fd)` — `setsockopt(SO_REUSEADDR)`. On failure: close fd, return `ERR_IO`.
8. `m_sock_ops->do_bind(m_sock_fd, config.bind_ip, config.bind_port)` — `bind()`. On failure: log FATAL, close fd, return `ERR_IO`.
9. `setup_dtls_config(config.tls)` called:

   **Inside `setup_dtls_config()`:**
   a. Asserts `tls_cfg.tls_enabled`, `tls_cfg.cert_file[0] != '\0'`.
   b. `m_ops->crypto_init()` — `psa_crypto_init()`. Fails → log FATAL, return `ERR_IO`.
   c. Determines `endpoint = MBEDTLS_SSL_IS_SERVER`.
   d. `m_ops->ssl_config_defaults(&m_ssl_conf, SERVER, MBEDTLS_SSL_TRANSPORT_DATAGRAM, DEFAULT)` — selects DTLS mode (REQ-6.4.1).
   e. `mbedtls_ssl_conf_handshake_timeout(&m_ssl_conf, 1000, 10000)` — DTLS retransmission: 1 s min, 10 s max.
   f. `mbedtls_ssl_conf_read_timeout(&m_ssl_conf, 100)` — per-poll 100 ms read timeout.
   g. Sets authmode (REQUIRED or NONE based on `verify_peer`).
   h. If `verify_peer && ca_file != ""`: parse CA cert, call `ssl_conf_ca_chain()`.
   i. `m_ops->x509_crt_parse_file(&m_cert, cert_file)`.
   j. `m_ops->pk_parse_keyfile(&m_pkey, key_file, nullptr)`.
   k. `m_ops->ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey)`.
   l. **DTLS cookie setup (REQ-6.4.2, server only):** `m_ops->ssl_cookie_setup(&m_cookie_ctx)` — initialises HMAC cookie context. Then `mbedtls_ssl_conf_dtls_cookies(&m_ssl_conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &m_cookie_ctx)`.
   m. Logs INFO "DTLS config ready". Returns `Result::OK`.

   On any failure in step 9: close fd, set `m_sock_fd = -1`, return the failure result.

10. Calls `server_wait_and_handshake()`.

    **Inside `server_wait_and_handshake()`:**
    a. Asserts `m_is_server`, `m_sock_fd >= 0`.
    b. `struct pollfd pfd; pfd.fd = m_sock_fd; pfd.events = POLLIN; poll(&pfd, 1, wait_ms)` — waits up to `connect_timeout_ms` ms (default 30 000 ms) for first client datagram. Returns ≤ 0 on timeout → log WARNING_HI, return `ERR_TIMEOUT`.
    c. `m_ops->recvfrom_peek(m_sock_fd, peek_buf, 1, (sockaddr*)&peer_addr, &peer_addr_len)` — `recvfrom(MSG_PEEK)` to discover client's (address, port) without consuming the datagram. Failure → return `ERR_IO`.
    d. `m_ops->net_connect(m_sock_fd, (sockaddr*)&peer_addr, peer_addr_len)` — POSIX `connect()` on the UDP socket, locking it to the single peer (single-peer model). Failure → return `ERR_IO`.
    e. `m_net_ctx.fd = m_sock_fd` — wires the POSIX fd into mbedTLS's net context for BIO callbacks.
    f. `m_ops->ssl_setup(&m_ssl, &m_ssl_conf)` — initialises the single DTLS session with the shared config. Failure → return `ERR_IO`.
    g. `mbedtls_ssl_set_bio(&m_ssl, &m_net_ctx, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout)` — wires BIO callbacks (Power of 10 Rule 9 deviation: library-defined symbols only).
    h. `mbedtls_ssl_set_timer_cb(&m_ssl, &m_timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay)` — wires retransmission timer (REQ-6.4.3, Rule 9 deviation).
    i. `mbedtls_ssl_set_mtu(&m_ssl, DTLS_MAX_DATAGRAM_BYTES)` — caps record size to 1400 bytes (REQ-6.4.4).
    j. `m_ops->ssl_set_client_transport_id(&m_ssl, (unsigned char*)&peer_addr, peer_addr_len)` — binds cookie to peer's transport identity (REQ-6.4.2). Failure → return `ERR_IO`.
    k. Calls `run_dtls_handshake(&peer_addr, peer_addr_len)`.

       **Inside `run_dtls_handshake()`:**
       - Asserts `peer_addr != nullptr`, `addr_len > 0`.
       - Loop (up to `DTLS_HANDSHAKE_MAX_ITER = 32` iterations):
         - `m_ops->ssl_handshake(&m_ssl)`:
           - Returns `0` → handshake complete, `done = true`.
           - Returns `MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED` → client sent ClientHello without valid cookie (first round). Server calls `mbedtls_ssl_session_reset()`, re-wires BIO and timer callbacks, re-sets transport ID, and loops (expects second ClientHello with cookie).
           - Returns `WANT_READ` or `WANT_WRITE` → partial flight; continue loop.
           - Any other error → log WARNING_HI, return `ERR_IO`.
       - If `!done` after `DTLS_HANDSHAKE_MAX_ITER`: log WARNING_HI, return `ERR_IO`.
       - Asserts `done`. Returns `Result::OK`.

    l. If `run_dtls_handshake()` fails: free `m_ssl`, re-init, return error result.
    m. `m_open = true`.
    n. Logs INFO "DTLS handshake complete (server): cipher=...". Asserts `m_open`. Returns `Result::OK`.

11. If `server_wait_and_handshake()` fails: close fd, `m_sock_fd = -1`, return error.
12. `init()` asserts `m_open`. Returns `Result::OK`.

---

## 4. Call Tree

```
DtlsUdpBackend::init()
 ├── m_recv_queue.init()
 ├── impairment_config_default()
 ├── m_impairment.init()
 ├── m_sock_ops->create_udp()          [socket()]
 ├── m_sock_ops->set_reuseaddr()       [setsockopt()]
 ├── m_sock_ops->do_bind()             [bind()]
 ├── setup_dtls_config()
 │    ├── m_ops->crypto_init()         [psa_crypto_init()]
 │    ├── m_ops->ssl_config_defaults() [DATAGRAM mode]
 │    ├── mbedtls_ssl_conf_handshake_timeout()
 │    ├── mbedtls_ssl_conf_read_timeout()
 │    ├── m_ops->x509_crt_parse_file() [CA cert, conditional]
 │    ├── m_ops->x509_crt_parse_file() [own cert]
 │    ├── m_ops->pk_parse_keyfile()
 │    ├── m_ops->ssl_conf_own_cert()
 │    ├── m_ops->ssl_cookie_setup()    [server only]
 │    └── mbedtls_ssl_conf_dtls_cookies()
 └── server_wait_and_handshake()
      ├── poll()                       [wait for first datagram]
      ├── m_ops->recvfrom_peek()       [MSG_PEEK]
      ├── m_ops->net_connect()         [connect UDP to peer]
      ├── m_ops->ssl_setup()
      ├── mbedtls_ssl_set_bio()
      ├── mbedtls_ssl_set_timer_cb()
      ├── mbedtls_ssl_set_mtu()
      ├── m_ops->ssl_set_client_transport_id()
      └── run_dtls_handshake()
           └── [loop] m_ops->ssl_handshake()
                       ├── on HELLO_VERIFY_REQUIRED:
                       │    ├── mbedtls_ssl_session_reset()
                       │    ├── mbedtls_ssl_set_bio()
                       │    ├── mbedtls_ssl_set_timer_cb()
                       │    └── m_ops->ssl_set_client_transport_id()
                       └── on 0: done
```

---

## 5. Key Components Involved

- **`DtlsUdpBackend`**: Owns all mbedTLS contexts (`m_ssl_conf`, `m_cert`, `m_ca_cert`, `m_pkey`, `m_ssl`, `m_cookie_ctx`, `m_timer`, `m_net_ctx`) and the POSIX UDP fd (`m_sock_fd`). Single-peer model: one `m_ssl` session.
- **`setup_dtls_config()`**: Configures DTLS (not TLS) mode via `MBEDTLS_SSL_TRANSPORT_DATAGRAM`; arms DTLS cookie anti-amplification on the server side.
- **`server_wait_and_handshake()`**: Blocks in `init()` until the first client datagram arrives and the DTLS handshake completes. This is the distinguishing behavior vs `TlsTcpBackend` where accept happens lazily in `receive_message()`.
- **`run_dtls_handshake()`**: Bounded handshake loop (`DTLS_HANDSHAKE_MAX_ITER=32`). Handles `HELLO_VERIFY_REQUIRED` (cookie exchange) by resetting the session and re-arming callbacks.
- **`IMbedtlsOps`** (`m_ops`): Injectable interface wrapping mbedTLS calls. Allows test injection of failure scenarios.
- **`ISocketOps`** (`m_sock_ops`): Injectable interface for POSIX socket calls.
- **`mbedtls_ssl_cookie_ctx`**: HMAC-based cookie generator/verifier. Cookie is bound to client `(address, port)` transport identity to prevent amplification attacks (REQ-6.4.2).
- **`mbedtls_timing_delay_context`**: Provides wall-clock timer for DTLS retransmission (RFC 6347 §4.2.4, REQ-6.4.3).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|---|---|---|
| `m_sock_ops->create_udp() < 0` | Log FATAL, return `ERR_IO` | Continue |
| `set_reuseaddr()` fails | Close fd, return `ERR_IO` | Continue |
| `do_bind()` fails | Log FATAL, close fd, return `ERR_IO` | Continue |
| `m_ops->crypto_init() != PSA_SUCCESS` | Log FATAL, return `ERR_IO` | Continue |
| `verify_peer && ca_file != ""` | Parse CA cert | Skip |
| Any cert/key/cookie setup fails | Return `ERR_IO` | Continue |
| `poll()` returns ≤ 0 | Log WARNING_HI, return `ERR_TIMEOUT` | Continue to peek |
| `recvfrom_peek()` < 0 | Return `ERR_IO` | Continue |
| `net_connect()` fails | Return `ERR_IO` | Continue |
| `ssl_handshake()` == 0 | Handshake complete | Continue loop |
| `ssl_handshake()` == `HELLO_VERIFY_REQUIRED` | Reset session, re-arm, loop | — |
| `ssl_handshake()` returns other error | Log, return `ERR_IO` | — |
| Loop completes without `done` | Return `ERR_IO` | (done=true, continue) |

---

## 7. Concurrency / Threading Behavior

- `init()` is synchronous and blocking: it does not return until the DTLS handshake completes or an error occurs. The `poll()` call inside `server_wait_and_handshake()` may block for up to `connect_timeout_ms` ms (default 30 s).
- All operations execute on the single caller thread. No threads are created.
- `m_recv_queue` (RingBuffer): SPSC contract; during `init()`, no consumer is active, so push/pop are single-threaded.
- No `std::atomic` operations in this flow. `mbedtls_timing_delay_context` uses its own internal state.
- The injected `IMbedtlsOps*` / `ISocketOps*` pointers must not be concurrently modified during `init()`.

---

## 8. Memory & Ownership Semantics

- **Stack buffers:**
  - `server_wait_and_handshake()`: `struct sockaddr_storage peer_addr` (128 bytes), `uint8_t peek_buf[1]` on stack.
  - `log_mbedtls_err()`: `char err_buf[128]` on helper stack.
- **Fixed static members:**
  - `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes): serialization/receive buffer.
- **Power of 10 Rule 3 deviation (init-phase):** `psa_crypto_init()`, cert parsing, and `ssl_handshake()` heap-allocate internally inside mbedTLS. Occurs only in `init()`, not on send/receive path. Documented in file header.
- **Ownership:** `DtlsUdpBackend` owns `m_sock_fd` (closed in `close()`); all mbedTLS contexts freed in destructor via `mbedtls_ssl_config_free()`, `mbedtls_x509_crt_free()`, `mbedtls_pk_free()`, `mbedtls_ssl_cookie_free()`, `mbedtls_psa_crypto_free()`.
- **`m_sock_ops`, `m_ops`:** Non-owning pointers; must outlive the backend instance.

---

## 9. Error Handling Flow

| Error condition | Result code | State consistency | Expected caller action |
|---|---|---|---|
| UDP socket/bind fails | `ERR_IO` | `m_sock_fd == -1`, `m_open == false` | `init()` returns error; do not use backend |
| PSA/cert/key setup fails | `ERR_IO` | `m_sock_fd` closed, `m_open == false` | Same |
| `poll()` timeout | `ERR_TIMEOUT` | `m_sock_fd` closed, `m_open == false` | Retry `init()` if appropriate |
| DTLS handshake fails | `ERR_IO` | `m_ssl` freed/re-init'd; `m_sock_fd` closed | Same; backend unusable |

All error paths close `m_sock_fd` before returning and leave `m_open == false`, so the system state is consistent for a subsequent `init()` attempt.

---

## 10. External Interactions

- **POSIX sockets:** `socket(AF_INET, SOCK_DGRAM, 0)`, `setsockopt(SO_REUSEADDR)`, `bind()`, `poll()`, `recvfrom(MSG_PEEK)`, `connect()` — all via `ISocketOps`.
- **mbedTLS:** `psa_crypto_init()`, `mbedtls_ssl_config_defaults(DATAGRAM)`, `mbedtls_x509_crt_parse_file()`, `mbedtls_pk_parse_keyfile()`, `mbedtls_ssl_cookie_setup()`, `mbedtls_ssl_handshake()` — via `IMbedtlsOps`.
- **File system:** `mbedtls_x509_crt_parse_file()` and `mbedtls_pk_parse_keyfile()` read PEM-format files from disk.

---

## 11. State Changes / Side Effects

| Member | Before `init()` | After successful `init()` |
|---|---|---|
| `m_open` | `false` | `true` |
| `m_is_server` | `false` | `true` |
| `m_tls_enabled` | `false` | `true` |
| `m_sock_fd` | `-1` | valid UDP fd (bound, connect()ed to peer) |
| `m_net_ctx.fd` | unset | `== m_sock_fd` |
| `m_ssl` | uninitialised | DTLS session established |
| `m_ssl_conf` | uninitialised | configured (DATAGRAM, cookies, certs) |
| `m_cookie_ctx` | uninitialised | HMAC cookie context ready |

---

## 12. Sequence Diagram

```
User                DtlsUdpBackend           ISocketOps        IMbedtlsOps/PSA     Client
 |                       |                       |                   |                |
 | init(config)          |                       |                   |                |
 |---------------------->|                       |                   |                |
 |                       | create_udp()          |                   |                |
 |                       |---------------------->|                   |                |
 |                       | set_reuseaddr()       |                   |                |
 |                       |---------------------->|                   |                |
 |                       | do_bind()             |                   |                |
 |                       |---------------------->|                   |                |
 |                       | crypto_init()         |                   |                |
 |                       |-----------------------------------------> |                |
 |                       | ssl_config_defaults(DATAGRAM)             |                |
 |                       |-----------------------------------------> |                |
 |                       | x509_crt_parse_file / pk_parse_keyfile    |                |
 |                       |-----------------------------------------> | (reads files)  |
 |                       | ssl_cookie_setup()                        |                |
 |                       |-----------------------------------------> |                |
 |                       | poll(listen_fd)       |                   |                |
 |                       |---------------------->| (blocking wait)   |                |
 |                       |                       |                   |  ClientHello   |
 |                       |                       | <-- datagram -------------------------
 |                       | recvfrom(MSG_PEEK)    |                   |                |
 |                       |---------------------->|                   |                |
 |                       | connect(peer_addr)    |                   |                |
 |                       |---------------------->|                   |                |
 |                       | ssl_setup()                               |                |
 |                       |-----------------------------------------> |                |
 |                       | ssl_set_bio/timer_cb/mtu                  |                |
 |                       |-----------------------------------------> |                |
 |                       | ssl_handshake() → HELLO_VERIFY_REQUIRED   |                |
 |                       |-----------------------------------------> |  HelloVerify   |
 |                       |                       |                   | ------------> |
 |                       |                       |                   |  ClientHello2 |
 |                       |                       |                   | <------------ |
 |                       | ssl_session_reset + re-arm                |                |
 |                       |-----------------------------------------> |                |
 |                       | ssl_handshake() loop → 0 (done)           |                |
 |                       |-----------------------------------------> |  [handshake]  |
 | <-- Result::OK ------- |                       |                   |                |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (`init()`):**
- The entire DTLS setup — socket creation, bind, PSA init, cert loading, cookie arming, and handshake — happens inside `init()` and must complete before `init()` returns.
- The server **blocks** in `poll()` waiting for the first client. This differs from `TlsTcpBackend` where the listen socket is non-blocking and `accept_and_handshake()` runs lazily.

**Runtime (after `init()`):**
- `send_message()` and `receive_message()` use the established `m_ssl` session for all data exchange.
- No further handshake occurs during steady-state operation.

---

## 14. Known Risks / Observations

- **Blocking `init()`:** The server will hang in `init()` indefinitely (up to `connect_timeout_ms`, default 30 s) if no client connects. This makes the server single-peer and sequential: it cannot serve a second client on the same `DtlsUdpBackend` instance.
- **`DTLS_HANDSHAKE_MAX_ITER = 32` ceiling:** If the handshake requires more than 32 retries (e.g., very high loss network), `run_dtls_handshake()` returns `ERR_IO` even if the handshake would eventually succeed. This is a conservative safety limit.
- **Single-peer model:** After `connect()`, the UDP socket only receives datagrams from the one peer. Datagrams from other addresses are silently filtered by the OS. There is no mechanism to accept a second client without creating a new `DtlsUdpBackend` instance.
- **`HELLO_VERIFY_REQUIRED` reset:** After session reset, BIO and timer callbacks must be re-wired. If this re-arm step is skipped, the handshake will fail. The current implementation handles this correctly in `run_dtls_handshake()`.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `mbedtls_ssl_session_reset()` on a datagram-mode SSL context fully re-initializes the handshake state, allowing the second ClientHello (with cookie) to be processed normally. This is inferred from mbedTLS DTLS documentation and the pattern in the source.
- `[ASSUMPTION]` `IMbedtlsOps::recvfrom_peek()` calls POSIX `recvfrom()` with `MSG_PEEK`. The implementation in `MbedtlsOpsImpl` is not shown but this is the only sensible implementation.
- `[ASSUMPTION]` The `connect_timeout_ms` field in `TransportConfig` defaults to `30000` when it is `0`. The code uses the ternary `config.connect_timeout_ms > 0 ? config.connect_timeout_ms : 30000`. Confirmed by reading the source directly.
