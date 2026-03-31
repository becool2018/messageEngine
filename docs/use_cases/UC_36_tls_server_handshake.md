# UC_36 — TLS server: bind, listen, accept, and complete TLS handshake with each client

**HL Group:** HL-18 — TLS-Encrypted TCP Transport
**Actor:** System (invoked automatically during User's call to `TlsTcpBackend::init()` and `receive_message()`)
**Requirement traceability:** REQ-4.1.1, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.3.4, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** User calls `TlsTcpBackend::init()` with `config.is_server = true` and `config.tls.tls_enabled = true`.

**Goal:** Bind a TCP listen socket, configure the mbedTLS shared ssl_conf object (loading certificate, private key, and CA), set the socket non-blocking, mark the transport open, and thereafter accept each connecting client and complete a TLS 1.2+ handshake before that client slot becomes usable for framed send/receive.

**Success outcome:** `init()` returns `Result::OK` with `m_open = true` and the listen socket bound. Each subsequent accepted client has `m_ssl[slot]` populated and a completed TLS handshake; `m_client_count` is incremented by one per accepted client.

**Error outcomes:**
- `psa_crypto_init()` fails → `init()` returns `Result::ERR_IO`; `m_open` remains false.
- `mbedtls_ssl_config_defaults()` fails → `Result::ERR_IO`.
- Certificate / key file load fails → `Result::ERR_IO`.
- `mbedtls_net_bind()` fails → `Result::ERR_IO`.
- `mbedtls_net_set_nonblock()` fails → listen socket freed; `Result::ERR_IO`.
- `accept_and_handshake()` TLS failure → client slot cleaned up; `m_client_count` unchanged; outer polling loop continues.

**System Internal note:** `accept_and_handshake()` is an internal helper called from `poll_clients_once()`, which is called from `receive_message()`. The user never calls either directly.

---

## 2. Entry Points

**Primary (User-facing):**
```
// src/platform/TlsTcpBackend.cpp, line 653
Result TlsTcpBackend::init(const TransportConfig& config)
```

**Internal sub-entry (called from receive_message → poll_clients_once):**
```
// src/platform/TlsTcpBackend.cpp, line 324
Result TlsTcpBackend::accept_and_handshake()
```

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase 1 — init() (server setup)**

1. `TlsTcpBackend::init(config)` is called. Assertions verify `config.kind == TransportKind::TCP` and `!m_open`.
2. `m_cfg`, `m_is_server`, `m_tls_enabled` are copied from `config`.
3. `m_recv_queue.init()` resets the inbound ring buffer.
4. `impairment_config_default(imp_cfg)` sets default impairment values; if `config.num_channels > 0`, channel 0's impairment config is used instead.
5. `m_impairment.init(imp_cfg)` initialises the impairment engine.
6. Because `m_tls_enabled == true`, `setup_tls_config(config.tls)` is called.
7. Inside `setup_tls_config()`:
   a. `psa_crypto_init()` initialises the PSA Crypto DRBG (init-phase heap allocation; documented Power of 10 Rule 3 deviation).
   b. `endpoint` is set to `MBEDTLS_SSL_IS_SERVER` because `tls_cfg.role == TlsRole::SERVER`.
   c. `mbedtls_ssl_config_defaults(&m_ssl_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)` configures the shared TLS object for TCP stream transport.
   d. `mbedtls_ssl_conf_authmode()` sets `MBEDTLS_SSL_VERIFY_REQUIRED` (if `verify_peer`) or `MBEDTLS_SSL_VERIFY_NONE`.
   e. If `verify_peer && ca_file[0] != '\0'`: `mbedtls_x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file)` loads the CA certificate; `mbedtls_ssl_conf_ca_chain()` registers it.
   f. `mbedtls_x509_crt_parse_file(&m_cert, tls_cfg.cert_file)` loads the server's own certificate chain.
   g. `mbedtls_pk_parse_keyfile(&m_pkey, tls_cfg.key_file, nullptr)` loads the server's private key.
   h. `mbedtls_ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey)` binds cert + key to the config object.
   i. Returns `Result::OK`; an INFO log entry is emitted.
8. Back in `init()`: because `m_is_server == true`, `bind_and_listen(config.bind_ip, config.bind_port)` is called.
9. Inside `bind_and_listen()`:
   a. `port_to_str(port, port_str)` converts the `uint16_t` port to a decimal string.
   b. `mbedtls_net_bind(&m_listen_net, ip, port_str, MBEDTLS_NET_PROTO_TCP)` creates the socket, sets `SO_REUSEADDR`, binds, and calls `listen()`.
   c. `mbedtls_net_set_nonblock(&m_listen_net)` makes the listen socket non-blocking so that `mbedtls_net_accept()` returns immediately when no client is pending.
   d. `m_open = true` is set.
   e. An INFO log entry is emitted.
   f. Returns `Result::OK`.
10. `init()` asserts `m_open` and returns `Result::OK`.

**Phase 2 — accept_and_handshake() (per connecting client, called from poll_clients_once)**

11. User calls `receive_message()` or the application event loop drives `poll_clients_once()`.
12. `poll_clients_once()` checks `m_is_server == true`. If `m_client_count == 0`, it calls `mbedtls_net_poll(&m_listen_net, MBEDTLS_NET_POLL_READ, timeout_ms)` to wait for an incoming connection without spinning.
13. `accept_and_handshake()` is called.
14. Assertions verify `m_is_server` and `m_listen_net.fd >= 0`.
15. If `m_client_count >= MAX_TCP_CONNECTIONS` (8), the function returns `Result::OK` without accepting (table full guard).
16. `slot = m_client_count` is calculated.
17. `mbedtls_net_accept(&m_listen_net, &m_client_net[slot], nullptr, 0U, nullptr)` accepts one pending connection. If no connection is pending (non-blocking), this returns `MBEDTLS_ERR_SSL_WANT_READ`; the function returns `Result::OK` silently.
18. Because `m_tls_enabled == true`:
    a. `mbedtls_net_set_block(&m_client_net[slot])` switches the accepted client fd to blocking mode (required for the TLS handshake's multi-round read/write sequence; the inherited non-blocking flag from the listen socket is cleared here).
    b. `mbedtls_ssl_setup(&m_ssl[slot], &m_ssl_conf)` attaches the shared config to this client's TLS context.
    c. `mbedtls_ssl_set_bio(&m_ssl[slot], &m_client_net[slot], mbedtls_net_send, mbedtls_net_recv, nullptr)` wires the TLS BIO to the client's net context.
    d. `mbedtls_ssl_handshake(&m_ssl[slot])` drives the full TLS handshake (ClientHello → ServerHello → Certificate → ServerHelloDone → ClientKeyExchange → ChangeCipherSpec → Finished). This call blocks until the handshake completes or fails (init-phase; Power of 10 Rule 3 deviation for mbedTLS internal allocations).
    e. On handshake failure: `mbedtls_ssl_free(&m_ssl[slot])`, `mbedtls_ssl_init(&m_ssl[slot])`, `mbedtls_net_free(&m_client_net[slot])`, `mbedtls_net_init(&m_client_net[slot])` clean up the slot; `Result::ERR_IO` is returned.
    f. On success: INFO log emitted with slot index and negotiated cipher suite (`mbedtls_ssl_get_ciphersuite()`).
19. `++m_client_count` — the slot is now active.
20. INFO log: "Accepted client N (TLS=ON), total=M".
21. Assertion: `m_client_count <= MAX_TCP_CONNECTIONS`.
22. Returns `Result::OK`.

---

## 4. Call Tree (Primary Success Path)

```
TlsTcpBackend::init()
 ├── m_recv_queue.init()
 ├── impairment_config_default()
 ├── m_impairment.init()
 ├── setup_tls_config()
 │    ├── psa_crypto_init()                         [PSA Crypto — init-phase heap]
 │    ├── mbedtls_ssl_config_defaults()             [STREAM, SERVER, PRESET_DEFAULT]
 │    ├── mbedtls_ssl_conf_authmode()
 │    ├── mbedtls_x509_crt_parse_file()  (CA cert)
 │    ├── mbedtls_ssl_conf_ca_chain()
 │    ├── mbedtls_x509_crt_parse_file()  (own cert)
 │    ├── mbedtls_pk_parse_keyfile()
 │    └── mbedtls_ssl_conf_own_cert()
 └── bind_and_listen()
      ├── port_to_str()
      ├── mbedtls_net_bind()                        [bind + listen]
      └── mbedtls_net_set_nonblock()

--- (later, from receive_message) ---

TlsTcpBackend::receive_message()
 └── poll_clients_once()
      ├── mbedtls_net_poll()             [wait for pending connection]
      └── accept_and_handshake()
           ├── mbedtls_net_accept()
           ├── mbedtls_net_set_block()  [switch client fd to blocking]
           ├── mbedtls_ssl_setup()
           ├── mbedtls_ssl_set_bio()
           └── mbedtls_ssl_handshake()  [full TLS handshake; blocking]
```

---

## 5. Key Components Involved

- **`TlsTcpBackend`** — top-level transport; holds all mbedTLS contexts as fixed member arrays (Power of 10 Rule 3).
- **`setup_tls_config()`** — configures the shared `mbedtls_ssl_config` and loads cert/key material; called once per `init()`.
- **`bind_and_listen()`** — creates the TCP listen socket via `mbedtls_net_bind()`, sets it non-blocking.
- **`accept_and_handshake()`** — called repeatedly from `poll_clients_once()`; accepts one client per call and, when TLS is enabled, drives the blocking TLS handshake to completion.
- **`poll_clients_once()`** — orchestrates both accept and per-client receive within `receive_message()`.
- **`mbedtls_ssl_config`** (`m_ssl_conf`) — shared TLS config object; populated once; referenced by all per-client `mbedtls_ssl_context` objects.
- **`mbedtls_net_context`** (`m_listen_net`, `m_client_net[]`) — wraps POSIX fds; used for mbedTLS BIO callbacks.
- **`mbedtls_ssl_context`** (`m_ssl[]`) — per-client TLS session state; at most `MAX_TCP_CONNECTIONS` (8) slots.
- **`ImpairmentEngine`** (`m_impairment`) — initialised but not exercised during the handshake phase.
- **`RingBuffer`** (`m_recv_queue`) — initialised to empty during `init()`; populated after handshake by `recv_from_client()`.
- **`Logger::log()`** — emits INFO on bind success and on each accepted client; emits WARNING_HI on handshake failure.

---

## 6. Branching Logic / Decision Points

| Condition | True path | False path |
|-----------|-----------|------------|
| `m_tls_enabled` at entry to `init()` | Call `setup_tls_config()` | Skip TLS setup; go directly to `bind_and_listen()` |
| `tls_cfg.verify_peer && ca_file[0] != '\0'` | Parse CA cert; register ca_chain | Skip CA cert loading; authmode still set |
| `psa_crypto_init() != PSA_SUCCESS` | Log FATAL; return `ERR_IO` | Continue |
| Any mbedtls_ssl_config call fails | Log WARNING_HI; return `ERR_IO` | Continue |
| `mbedtls_net_bind()` fails | Log WARNING_HI; return `ERR_IO` | Continue to `set_nonblock` |
| `mbedtls_net_set_nonblock()` fails | Free listen socket; return `ERR_IO` | Set `m_open = true`; return OK |
| `m_client_count >= MAX_TCP_CONNECTIONS` | Return OK (table full; no accept) | Proceed to `mbedtls_net_accept()` |
| `mbedtls_net_accept() != 0` | Return OK (no pending connection or error) | Proceed to TLS handshake |
| `m_tls_enabled` in `accept_and_handshake()` | Call set_block / ssl_setup / ssl_handshake | Skip TLS; just increment `m_client_count` |
| `mbedtls_net_set_block()` fails | Clean up slot; return `ERR_IO` | Call `ssl_setup()` |
| `mbedtls_ssl_setup()` fails | Clean up slot; return `ERR_IO` | Call `ssl_set_bio()` then `ssl_handshake()` |
| `mbedtls_ssl_handshake() != 0` | ssl_free/ssl_init/net_free/net_init; return `ERR_IO` | `++m_client_count`; log INFO; return OK |
| `m_client_count == 0` in `poll_clients_once()` | Call `mbedtls_net_poll()` to avoid spin | Skip poll; call `accept_and_handshake()` directly |

---

## 7. Concurrency / Threading Behavior

All operations in this use case execute on the single thread that calls `TlsTcpBackend::init()` and later `receive_message()`. No additional threads are created by `TlsTcpBackend`.

The TLS handshake in `accept_and_handshake()` is blocking on the caller's thread. The listen socket is non-blocking so that `mbedtls_net_accept()` does not block when no client is pending; only the accepted client fd is switched to blocking mode before the handshake.

No `std::atomic`, mutex, or other synchronisation primitive is used in this flow. If the caller drives `receive_message()` from multiple threads, they provide their own external synchronisation (not enforced by this class).

---

## 8. Memory and Ownership Semantics

**Stack allocations in `setup_tls_config()`:**
- `char err_buf[128]` inside `log_mbedtls_err()` (file-local helper).
- No heap allocations by application code.

**Stack allocations in `bind_and_listen()`:**
- `char port_str[6]` — decimal port string; maximum 5 digits + NUL.

**mbedTLS internal heap allocations (init-phase, Power of 10 Rule 3 deviation):**
- `psa_crypto_init()` allocates the PSA Crypto entropy/DRBG state on the heap inside the mbedTLS library.
- `mbedtls_x509_crt_parse_file()` heap-allocates parsed DER certificate data inside `m_cert` / `m_ca_cert`.
- `mbedtls_pk_parse_keyfile()` heap-allocates parsed key material inside `m_pkey`.
- `mbedtls_ssl_handshake()` heap-allocates the handshake state machine and record buffers inside `m_ssl[slot]`.
- All of the above occur exclusively during init / accept — never on the send/receive critical path.

**Fixed member arrays (stack-like lifetime, Power of 10 Rule 3):**
- `m_client_net[MAX_TCP_CONNECTIONS]` (8 × `mbedtls_net_context`) — embedded in the `TlsTcpBackend` object.
- `m_ssl[MAX_TCP_CONNECTIONS]` (8 × `mbedtls_ssl_context`) — embedded in the `TlsTcpBackend` object.
- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes) — embedded; used for send/receive framing.

**Ownership:**
- `TlsTcpBackend` owns all mbedTLS contexts. The destructor calls `TlsTcpBackend::close()` followed by `mbedtls_ssl_config_free()`, `mbedtls_x509_crt_free()`, `mbedtls_pk_free()`, `mbedtls_net_free()` for each context, and `mbedtls_psa_crypto_free()`.
- On handshake failure, the failed client slot is immediately freed with `mbedtls_ssl_free()` + `mbedtls_ssl_init()` + `mbedtls_net_free()` + `mbedtls_net_init()` before returning.
- `m_sock_ops` is a non-owning raw pointer; it must outlive the `TlsTcpBackend` instance.

**`SOCKET_RECV_BUF_BYTES` = 8192; `MAX_TCP_CONNECTIONS` = 8.**

---

## 9. Error Handling Flow

| Error event | mbedTLS / POSIX call | State after | Caller action |
|-------------|---------------------|-------------|---------------|
| `psa_crypto_init()` != PSA_SUCCESS | — | `m_open = false` | `init()` returns `ERR_IO`; user should not proceed |
| `ssl_config_defaults()` != 0 | — | `m_open = false` | `init()` returns `ERR_IO` |
| `x509_crt_parse_file()` (CA) fails | — | `m_open = false` | `init()` returns `ERR_IO` |
| `x509_crt_parse_file()` (own cert) fails | — | `m_open = false` | `init()` returns `ERR_IO` |
| `pk_parse_keyfile()` fails | — | `m_open = false` | `init()` returns `ERR_IO` |
| `ssl_conf_own_cert()` fails | — | `m_open = false` | `init()` returns `ERR_IO` |
| `mbedtls_net_bind()` fails | — | `m_open = false`; listen fd not opened | `init()` returns `ERR_IO` |
| `mbedtls_net_set_nonblock()` fails | — | listen fd freed; `m_open = false` | `init()` returns `ERR_IO` |
| `mbedtls_net_accept()` returns non-zero | WANT_READ or other | no slot allocated | `accept_and_handshake()` returns `OK`; poll loop retries |
| `mbedtls_net_set_block()` fails on client | — | slot freed; `m_client_count` unchanged | `accept_and_handshake()` returns `ERR_IO`; poll loop continues |
| `mbedtls_ssl_setup()` fails on client | — | slot freed | `accept_and_handshake()` returns `ERR_IO` |
| `mbedtls_ssl_handshake()` fails | any fatal TLS alert | slot freed (ssl_free + net_free + re-init) | `accept_and_handshake()` returns `ERR_IO`; next poll attempt may accept a new client |

All errors are logged via `log_mbedtls_err()` at WARNING_HI severity. Errors in `accept_and_handshake()` do not propagate to `init()` because they happen after `init()` has already returned `OK`.

---

## 10. External Interactions

| Call | Purpose | Library / OS |
|------|---------|--------------|
| `psa_crypto_init()` | Initialise PSA Crypto entropy/DRBG | mbedTLS 4.0 / PSA Crypto |
| `mbedtls_ssl_config_defaults()` | Set STREAM/SERVER/PRESET_DEFAULT | mbedTLS |
| `mbedtls_ssl_conf_authmode()` | Set peer verification mode | mbedTLS |
| `mbedtls_x509_crt_parse_file()` | Parse PEM cert file from disk | mbedTLS + file I/O |
| `mbedtls_ssl_conf_ca_chain()` | Register CA cert with ssl_conf | mbedTLS |
| `mbedtls_pk_parse_keyfile()` | Parse PEM private key from disk | mbedTLS + file I/O |
| `mbedtls_ssl_conf_own_cert()` | Bind cert + key to ssl_conf | mbedTLS |
| `mbedtls_net_bind()` | Create TCP socket; bind; listen | mbedTLS (wraps POSIX socket/bind/listen) |
| `mbedtls_net_set_nonblock()` | `fcntl(O_NONBLOCK)` on listen fd | mbedTLS (wraps fcntl) |
| `mbedtls_net_poll()` | `poll()` on listen fd | mbedTLS (wraps poll) |
| `mbedtls_net_accept()` | `accept()` on listen fd | mbedTLS (wraps accept) |
| `mbedtls_net_set_block()` | Clear `O_NONBLOCK` on client fd | mbedTLS (wraps fcntl) |
| `mbedtls_ssl_setup()` | Associate ssl_conf with ssl_context | mbedTLS |
| `mbedtls_ssl_set_bio()` | Wire send/recv callbacks to net_context | mbedTLS |
| `mbedtls_ssl_handshake()` | Drive full TLS handshake state machine | mbedTLS |
| `mbedtls_ssl_get_ciphersuite()` | Read negotiated cipher name for logging | mbedTLS |
| `mbedtls_strerror()` | Convert error code to human-readable string | mbedTLS |

File I/O for certificate and key files occurs only in `setup_tls_config()` during `init()`.

---

## 11. State Changes / Side Effects

| What changes | When | Before → After |
|---|---|---|
| `m_tls_enabled` | `init()` entry | `false` → `config.tls.tls_enabled` |
| `m_is_server` | `init()` entry | `false` → `true` |
| `m_cfg` | `init()` entry | zeroed → copy of `config` |
| `m_ssl_conf` | `setup_tls_config()` | zero-initialised (ctor) → fully configured STREAM/SERVER config |
| `m_cert` | `setup_tls_config()` | empty → parsed certificate chain |
| `m_ca_cert` | `setup_tls_config()` (conditional) | empty → parsed CA certificate |
| `m_pkey` | `setup_tls_config()` | empty → parsed private key |
| `m_listen_net.fd` | `bind_and_listen()` | -1 → valid POSIX fd (bound, listening) |
| `m_open` | `bind_and_listen()` | `false` → `true` |
| `m_client_net[slot].fd` | `mbedtls_net_accept()` success | -1 → valid accepted fd |
| `m_ssl[slot]` | `mbedtls_ssl_setup()` + `ssl_handshake()` | zero-initialised → fully established TLS session |
| `m_client_count` | `accept_and_handshake()` success | N → N+1 |

The PSA Crypto global state is initialised as a side effect of `psa_crypto_init()` and freed by `mbedtls_psa_crypto_free()` in the destructor.

---

## 12. Sequence Diagram

```
User              TlsTcpBackend        mbedTLS / PSA Crypto      File I/O (certs)
  |                     |                      |                        |
  |--- init(config) --->|                      |                        |
  |                     |--- psa_crypto_init() -->|                     |
  |                     |<-- PSA_SUCCESS ---------|                     |
  |                     |--- ssl_config_defaults() -->|                 |
  |                     |<-- 0 -------------------|                     |
  |                     |--- ssl_conf_authmode() -->|                   |
  |                     |                      |   x509_crt_parse_file(CA) -->|
  |                     |                      |<-- 0 -------------------|
  |                     |--- ssl_conf_ca_chain() -->|                   |
  |                     |   x509_crt_parse_file(cert) -->|              |
  |                     |                      |<-- 0 -------------------|
  |                     |   pk_parse_keyfile() -->|                     |
  |                     |                      |<-- 0 -------------------|
  |                     |--- ssl_conf_own_cert() -->|                   |
  |                     |<-- 0 -------------------|                     |
  |                     |--- net_bind() --------->|                     |
  |                     |<-- 0 (bound+listening) -|                     |
  |                     |--- net_set_nonblock() -->|                    |
  |                     |<-- 0 -------------------|                     |
  |                     | (m_open = true)                               |
  |<-- Result::OK -------|                      |                        |
  |                     |                      |                        |
  |--- receive_message() ->|                   |                        |
  |                     | (poll_clients_once)  |                        |
  |                     |--- net_poll() ------->|  [wait for client]    |
  |                     |<-- POLLIN ------------|                       |
  |                     |--- net_accept() ------>|                      |
  |                     |<-- 0 (slot 0, fd=N) ---|                     |
  |                     |--- net_set_block() ---->|                     |
  |                     |<-- 0 ------------------|                     |
  |                     |--- ssl_setup(slot 0) -->|                     |
  |                     |<-- 0 ------------------|                     |
  |                     |--- ssl_set_bio() ------>|                     |
  |                     |--- ssl_handshake() ----->|  [TLS 1.2 flights] |
  |                     |<-- 0 (handshake done) --|                     |
  |                     | (m_client_count = 1)  |                        |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (init(), called once):**
- PSA Crypto is initialised.
- The shared `mbedtls_ssl_config` is fully configured and cert/key material is loaded from disk.
- The TCP listen socket is bound and set non-blocking.
- `m_open` is set to `true`.
- No impairment is applied; no messages are sent or received.

**Runtime (accept_and_handshake(), called repeatedly from receive_message):**
- On each call to `receive_message()`, `poll_clients_once()` checks for a pending connection.
- If a connection is pending, `accept_and_handshake()` accepts it and runs the TLS handshake (blocking on the caller's thread until complete).
- After the handshake, `recv_from_client()` reads framed messages from the newly established TLS session.
- The `m_client_count` tracks how many TLS sessions are live at any given moment.

---

## 14. Known Risks / Observations

- **Blocking handshake in receive_message:** `mbedtls_ssl_handshake()` inside `accept_and_handshake()` is blocking. If a misbehaving client initiates a connection but stalls the TLS handshake, the call to `receive_message()` will block for however long the handshake takes before the per-record read times out internally. There is no application-level timeout wrapping the handshake call.
- **Table compaction on remove:** `remove_client(idx)` shifts array entries left by value-copying `mbedtls_net_context` and `mbedtls_ssl_context` structs. If any mbedTLS internal pointer within those structs points back into the struct itself, the copy will produce a dangling pointer. [ASSUMPTION] The mbedTLS 4.0 implementations of these contexts do not contain self-referential internal pointers, but this should be verified against the mbedTLS source.
- **Non-blocking accept with blocking handshake:** After `mbedtls_net_accept()` returns a valid client fd, `mbedtls_net_set_block()` is called before the handshake. If that call fails (e.g., bad fd), the slot is cleaned up and `ERR_IO` is returned, but the underlying TCP connection is already established. The client will see a TCP RST or a closed connection.
- **One handshake per `receive_message` call:** `poll_clients_once()` calls `accept_and_handshake()` exactly once per invocation. If multiple clients connect simultaneously, each call to `receive_message()` processes at most one new client. This is by design to keep per-call work bounded, but may add latency if many clients connect at once.
- **MAX_TCP_CONNECTIONS ceiling:** When `m_client_count == MAX_TCP_CONNECTIONS` (8), incoming connections are silently ignored (the accept is not called). The connecting peer will remain in the OS accept queue until a slot frees or the OS queue fills.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `mbedtls_net_context` and `mbedtls_ssl_context` do not contain self-referential pointers in mbedTLS 4.0, making value-copy in `remove_client()` safe.
- [ASSUMPTION] `config.tls.role == TlsRole::SERVER` is set by the caller when `config.is_server == true`. The code checks `tls_cfg.role == TlsRole::SERVER` independently to select the mbedTLS endpoint constant.
- [ASSUMPTION] The TLS handshake completing successfully implies that certificate verification has passed (when `verify_peer == true`). No additional application-level certificate check is performed after `ssl_handshake()` returns 0.
- [ASSUMPTION] `mbedtls_ssl_handshake()` is re-entrant with respect to multiple distinct `mbedtls_ssl_context` objects simultaneously (different client slots). Each slot has its own context, so there is no shared mutable state between them at the session level.
- [ASSUMPTION] The PSA Crypto global state initialised by `psa_crypto_init()` is safe to share across all client sessions; it is not per-session.
