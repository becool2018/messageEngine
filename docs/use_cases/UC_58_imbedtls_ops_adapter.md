# UC_58 — IMbedtlsOps / MbedtlsOpsImpl: injectable mbedTLS adapter for DtlsUdpBackend fault injection in tests

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-6.4.1, REQ-6.4.2, REQ-6.3.4

This is a System Internal use case. `IMbedtlsOps` wraps the mbedTLS API calls used by `DtlsUdpBackend` to enable fault injection in unit tests without a real TLS/DTLS stack. `MbedtlsOpsImpl` is the production implementation. The User never calls these directly.

---

## 1. Use Case Overview

- **Trigger:** `DtlsUdpBackend` is constructed with an `IMbedtlsOps*` parameter (dependency injection). Every mbedTLS call in `DtlsUdpBackend` is dispatched through this interface.
- **Goal:** Allow test code to inject failures at any mbedTLS call point (e.g., `mbedtls_ssl_handshake()` returning `MBEDTLS_ERR_SSL_CONN_EOF`, `mbedtls_x509_crt_parse_file()` returning `MBEDTLS_ERR_X509_FILE_IO_ERROR`) to achieve branch coverage of DTLS error paths.
- **Success outcome (production):** `MbedtlsOpsImpl` is injected; all methods delegate directly to the corresponding mbedTLS API.
- **Error outcomes:** Each method returns the mbedTLS return value (0 on success, negative error code on failure). `DtlsUdpBackend` checks all return values.

---

## 2. Entry Points

```cpp
// src/platform/IMbedtlsOps.hpp
class IMbedtlsOps {
    virtual int ssl_handshake(mbedtls_ssl_context* ssl) = 0;
    virtual int ssl_write(mbedtls_ssl_context* ssl, const uint8_t* buf, size_t len) = 0;
    virtual int ssl_read(mbedtls_ssl_context* ssl, uint8_t* buf, size_t len) = 0;
    virtual int x509_crt_parse_file(mbedtls_x509_crt* crt, const char* path) = 0;
    virtual int pk_parse_keyfile(mbedtls_pk_context* pk, const char* path,
                                  const char* pwd, ...) = 0;
    virtual int ssl_cookie_setup(mbedtls_ssl_cookie_ctx* ctx, ...) = 0;
    // ... additional mbedTLS function wrappers
};

// src/platform/MbedtlsOpsImpl.hpp
class MbedtlsOpsImpl : public IMbedtlsOps { /* delegates to mbedTLS API */ };
```

---

## 3. End-to-End Control Flow

**Production path (MbedtlsOpsImpl):**
Each method is a direct delegation:
- `ssl_handshake(ssl)`: `return mbedtls_ssl_handshake(ssl)`.
- `ssl_write(ssl, buf, len)`: `return mbedtls_ssl_write(ssl, buf, (int)len)`.
- `ssl_read(ssl, buf, len)`: `return mbedtls_ssl_read(ssl, buf, (int)len)`.
- `x509_crt_parse_file(crt, path)`: `return mbedtls_x509_crt_parse_file(crt, path)`.
- `pk_parse_keyfile(pk, path, pwd, ...)`: `return mbedtls_pk_parse_keyfile(pk, path, pwd, ...)`.
- `ssl_cookie_setup(ctx, ...)`: `return mbedtls_ssl_cookie_setup(ctx, ...)`.

**Test path (MockMbedtlsOps):**
- `ssl_handshake()` can return `MBEDTLS_ERR_SSL_CONN_EOF` to simulate handshake failure.
- `x509_crt_parse_file()` can return `MBEDTLS_ERR_X509_FILE_IO_ERROR` to simulate missing cert.
- This enables coverage of: cert load failure branch (`ERR_INVALID`), handshake failure branch (log + free ssl), WANT_READ/WANT_WRITE retry branch.

---

## 4. Call Tree

```
DtlsUdpBackend::accept_dtls_client()              [DtlsUdpBackend.cpp]
 └── m_mbedtls->ssl_handshake(&m_ssl)             [IMbedtlsOps vtable]
      └── MbedtlsOpsImpl::ssl_handshake()
           └── mbedtls_ssl_handshake(&m_ssl)      [mbedTLS API]

DtlsUdpBackend::load_tls_certs()                  [DtlsUdpBackend.cpp]
 └── m_mbedtls->x509_crt_parse_file(&m_cert, path)
      └── MbedtlsOpsImpl::x509_crt_parse_file()
           └── mbedtls_x509_crt_parse_file()
```

---

## 5. Key Components Involved

- **`IMbedtlsOps`** — pure-virtual interface; the only mbedTLS-calling point in `DtlsUdpBackend`. Enables complete isolation of the backend from the real mbedTLS stack in tests.
- **`MbedtlsOpsImpl`** — production singleton; delegates each method to the corresponding mbedTLS 4.0 PSA API call.
- **Virtual dispatch** — Rule 9 exception for vtable-backed virtual functions; approved for transport polymorphism in this codebase.
- **Fault injection scope** — the interface covers all mbedTLS calls that can return errors: cert loading, key loading, cookie setup, handshake, send, receive.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `IMbedtlsOps*` is `MbedtlsOpsImpl` | Real mbedTLS calls | Mock returns configured result |
| mbedTLS returns `WANT_READ`/`WANT_WRITE` | Retry in handshake loop | Success (0) or fatal |
| mbedTLS returns fatal error | Log WARNING_LO; return ERR_IO | Continue |

---

## 7. Concurrency / Threading Behavior

- `DtlsUdpBackend` uses one DTLS session; all `IMbedtlsOps` calls are single-threaded.
- `MbedtlsOpsImpl` is stateless; thread-safe to the extent that mbedTLS itself is (per-context calls are safe from one thread).
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `MbedtlsOpsImpl` — stateless; no member variables; no heap allocation.
- The `IMbedtlsOps*` pointer is injected at `DtlsUdpBackend` construction; the backend does not own it.
- mbedTLS contexts (`m_ssl`, `m_ssl_conf`, etc.) are owned by `DtlsUdpBackend` and passed to `IMbedtlsOps` methods by pointer.

---

## 9. Error Handling Flow

- Each `IMbedtlsOps` method returns the mbedTLS integer return code.
- `DtlsUdpBackend` checks all return values (Power of 10 Rule 7).
- On non-zero (non-WANT) return: log at WARNING_LO; classify as `ERR_IO` or `ERR_INVALID`; clean up SSL context.

---

## 10. External Interactions

**Production (`MbedtlsOpsImpl`):**
- **mbedTLS 4.0 PSA API:** All mbedTLS API calls occur through this layer.
- **File I/O:** `x509_crt_parse_file()` and `pk_parse_keyfile()` read PEM files.
- **PSA Crypto / entropy:** mbedTLS handshake uses PSA entropy; initialized via `psa_crypto_init()` (called directly from `DtlsUdpBackend::init()`, not through `IMbedtlsOps`).

---

## 11. State Changes / Side Effects

`MbedtlsOpsImpl` is stateless. All side effects are in the mbedTLS context objects (`m_ssl`, `m_ssl_conf`, `m_cert`, etc.) owned by `DtlsUdpBackend`.

---

## 12. Sequence Diagram

```
[DtlsUdpBackend constructor — production]
  DtlsUdpBackend(ISocketOps* s, IMbedtlsOps* m) : m_sock_ops(s), m_mbedtls(m) {}

[DtlsUdpBackend::accept_dtls_client() — production]
  -> m_mbedtls->ssl_handshake(&m_ssl)             [vtable dispatch]
       -> MbedtlsOpsImpl::ssl_handshake()
            -> mbedtls_ssl_handshake(&m_ssl)
            <- 0 (success)
  [m_has_session = true]

[DtlsUdpBackend::accept_dtls_client() — test with mock]
  -> m_mbedtls->ssl_handshake(&m_ssl)             [vtable dispatch]
       -> MockMbedtlsOps::ssl_handshake()         [returns MBEDTLS_ERR_SSL_CONN_EOF]
  [DtlsUdpBackend logs WARNING_LO; clears SSL; returns ERR_IO]
  [Branch covered: handshake failure path]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `IMbedtlsOps*` injected at `DtlsUdpBackend` construction.
- For production: `MbedtlsOpsImpl::instance()` or stack-allocated `MbedtlsOpsImpl`.
- For tests: a mock configured with the desired mbedTLS error sequence.

**Runtime:**
- Called during `init()` (cert loading, SSL config) and during `accept_dtls_client()` / `send_message()` / `receive_message()` (handshake, write, read).

---

## 14. Known Risks / Observations

- **Vtable overhead for high-frequency calls:** `ssl_write()` and `ssl_read()` are called on every `send_message()` and `receive_message()`. The virtual dispatch overhead is negligible compared to TLS record encryption/decryption latency.
- **Interface completeness:** If a new mbedTLS API call is added to `DtlsUdpBackend` without adding it to `IMbedtlsOps`, that call cannot be fault-injected in tests. The interface must be kept in sync with the backend's mbedTLS usage.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `IMbedtlsOps` covers the specific mbedTLS calls used by `DtlsUdpBackend`; exact method set inferred from the DTLS flow documented in UC_38 and UC_39.
- `[ASSUMPTION]` `MbedtlsOpsImpl` is a stateless class with all methods as one-line delegations to mbedTLS API functions.
- `[ASSUMPTION]` `psa_crypto_init()` is called directly (not through `IMbedtlsOps`) since it is a one-time initialization call; wrapping it would add complexity without enabling meaningful fault injection.
