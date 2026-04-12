# UC_75 — TLS Session Store Lifecycle: TlsSessionStore saves, presents, and zeroes TLS session material

**HL Group:** HL-25: TLS Session Resumption on Reconnect — System Internals sub-function
**Actor:** User (creates and owns `TlsSessionStore`); System (`TlsTcpBackend` reads/writes it)
**Requirement traceability:** REQ-6.3.4

---

## 1. Use Case Overview

- **Trigger:** User constructs a `TlsSessionStore`, injects it into a `TlsTcpBackend` via
  `set_session_store()` before calling `init()`, then calls `close()` and reuses the same
  store with a second `TlsTcpBackend` instance to resume the session without a full handshake.
- **Goal:** Persist a single TLS session ticket across `close()` / `init()` cycles so
  that the `try_load_client_session()` branch in `TlsTcpBackend::init()` is reachable
  (Class B M5 coverage) and the application benefits from reduced reconnection latency.
- **Success outcome (save):** `try_save_client_session()` writes session material into
  `store.session` and sets `store.session_valid = true` after a successful first handshake.
- **Success outcome (load):** `try_load_client_session()` calls `mbedtls_ssl_set_session()`
  with the saved ticket; the TLS handshake completes as a session resumption (abbreviated handshake).
- **Cleanup:** Caller calls `store.zeroize()` when the session is no longer needed.
  `~TlsSessionStore()` calls `zeroize()` as a safety net.

---

## 2. Entry Points

```cpp
// src/platform/TlsSessionStore.hpp/.cpp — caller-owned value type
TlsSessionStore::TlsSessionStore();   // default-construct; session_valid = false
TlsSessionStore::zeroize();           // SC: HAZ-012, HAZ-017

// src/platform/TlsTcpBackend.cpp — called by TlsTcpBackend::init()
void TlsTcpBackend::set_session_store(TlsSessionStore* store);
void TlsTcpBackend::try_save_client_session();
void TlsTcpBackend::try_load_client_session();
```

---

## 3. End-to-End Control Flow

**First connection (save):**

1. User declares `TlsSessionStore store;` (stack or member).
2. User calls `client.set_session_store(&store)` before `client.init(cfg)`.
3. `TlsTcpBackend::init()` calls `try_load_client_session()`:
   - `m_session_store_ptr == nullptr` or `!store.session_valid` → no-op.
4. Handshake completes successfully.
5. `TlsTcpBackend::init()` calls `try_save_client_session()`:
   - `m_session_store_ptr != nullptr` && `session_resumption_enabled`.
   - `mbedtls_ssl_get_session(&m_ssl, &store.session)` — copies session material into store.
   - `store.session_valid.store(true)` on success.
6. Application uses the connection; calls `client.close()`.
7. `close()` does NOT clear `store.session` — the caller owns the store.

**Second connection (load):**

8. User constructs a second `TlsTcpBackend client2` (or re-uses the same instance after close).
9. `client2.set_session_store(&store)` — same store, `session_valid == true`.
10. `TlsTcpBackend::init()` calls `try_load_client_session()`:
    - `store.session_valid == true` → `mbedtls_ssl_set_session(&m_ssl, &store.session)`.
    - Handshake completes as session resumption (abbreviated; no full certificate exchange).
11. `try_save_client_session()` updates `store.session` with the refreshed ticket (if server supports ticket rotation).

**Cleanup:**

12. User calls `store.zeroize()`:
    - `mbedtls_ssl_session_free(&store.session)` — releases any embedded pointers.
    - `mbedtls_platform_zeroize(&store.session, sizeof(mbedtls_ssl_session))` — secure wipe (CWE-14).
    - `mbedtls_ssl_session_init(&store.session)` — return to clean state.
    - `store.session_valid.store(false)`.

---

## 4. Call Tree

```
[First init]
TlsTcpBackend::init()
 ├── set_session_store(&store)
 ├── try_load_client_session()           [no-op: session_valid==false]
 ├── client_connect_and_handshake()      [full TLS handshake]
 └── try_save_client_session()
      └── mbedtls_ssl_get_session()      [copy session → store.session]
          store.session_valid = true

[Second init]
TlsTcpBackend::init()
 ├── try_load_client_session()           [session_valid==true]
 │    └── mbedtls_ssl_set_session()      [present ticket to server]
 └── client_connect_and_handshake()      [abbreviated handshake]

[Cleanup]
store.zeroize()
 ├── mbedtls_ssl_session_free()
 ├── mbedtls_platform_zeroize()          [secure wipe; §7c]
 └── mbedtls_ssl_session_init()
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `TlsSessionStore` | Caller-owned value type; holds `mbedtls_ssl_session` + `session_valid` flag |
| `session_valid` (`std::atomic<bool>`) | Thread-safe flag; prevents data race between app thread `zeroize()` and backend thread load |
| `try_save_client_session()` | Writes session after handshake; sets `session_valid` |
| `try_load_client_session()` | Presents saved session before handshake; enables abbreviation |
| `zeroize()` | Secure wipe using `mbedtls_platform_zeroize` (never `memset`; §7c) |
| `~TlsSessionStore()` | Calls `zeroize()` as a safety net |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `m_session_store_ptr == nullptr` | `try_load` / `try_save` are no-ops |
| `!session_resumption_enabled` | `try_save` is skipped |
| `!store.session_valid` | `try_load` is a no-op; full handshake used |
| `store.session_valid == true` | `try_load` calls `mbedtls_ssl_set_session()` |
| `mbedtls_ssl_get_session()` fails | `session_valid` remains false; log WARNING_LO |

---

## 7. Concurrency / Threading Behavior

`store.session_valid` is `std::atomic<bool>` (CLAUDE.md §1d carve-out). This guards the
flag against a data race between a background backend thread calling `try_load_client_session()`
and an application thread calling `zeroize()`. The `session` struct itself is not atomically
protected — the caller must not call `zeroize()` concurrently with any backend operation
that dereferences `m_session_store_ptr`.

---

## 8. Memory & Ownership Semantics

- `TlsSessionStore` is non-copyable and non-movable (`mbedtls_ssl_session` contains
  internal pointers).
- No heap: `mbedtls_ssl_session` is embedded directly as a fixed-size value member
  (Power of 10 Rule 3).
- Caller owns the store's lifetime; `TlsTcpBackend` holds only a raw pointer (`m_session_store_ptr`);
  the backend must not outlive the store.

---

## 9. Error Handling Flow

```
try_load_client_session():
  mbedtls_ssl_set_session() fails → log WARNING_LO; proceed to full handshake

try_save_client_session():
  mbedtls_ssl_get_session() fails → log WARNING_LO; session_valid stays false

zeroize():
  No error return; always runs to completion.
```

---

## 10. External Interactions

- `mbedtls_ssl_session_free()`, `mbedtls_platform_zeroize()`, `mbedtls_ssl_session_init()` —
  mbedTLS API calls (via `MbedtlsOpsImpl` in production; injectable via `IMbedtlsOps` in tests).

---

## 11. State Changes / Side Effects

- `store.session_valid`: false → true after successful save; true → false after `zeroize()`.
- `store.session`: overwritten by `mbedtls_ssl_get_session()` on save; zeroed by `zeroize()`.

---

## 12. Sequence Diagram

```
User                    TlsTcpBackend           TlsSessionStore
  -> set_session_store(&store)
  -> init(cfg)
       -> try_load_client_session()   [no-op; session_valid=false]
       -> handshake (full)
       -> try_save_client_session()
            -> mbedtls_ssl_get_session() -> store.session filled
            -> store.session_valid = true
  <- OK

  -> close()

User                    TlsTcpBackend2          TlsSessionStore (same)
  -> set_session_store(&store)
  -> init(cfg)
       -> try_load_client_session()   [session_valid=true]
            -> mbedtls_ssl_set_session() -> session ticket presented
       -> handshake (abbreviated)
  <- OK

  -> store.zeroize()                 [secure wipe; session_valid=false]
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** `TlsSessionStore()` default-constructor zero-inits via
  `mbedtls_ssl_session_init()` and sets `session_valid = false`.
- **Runtime:** `try_load` is called once per `TlsTcpBackend::init()`; `try_save` is called
  once after each successful handshake.

---

## 14. Known Risks / Observations

- **Caller lifetime responsibility:** If `TlsSessionStore` goes out of scope while
  `TlsTcpBackend` still holds `m_session_store_ptr`, the next `try_load` or `try_save`
  is a use-after-free. The user must ensure the store outlives all backends that reference it.
- **Security contract:** Relying on `~TlsSessionStore()` for zeroing means session key
  material survives until the destructor runs. Applications with strict key-material lifetime
  requirements must call `zeroize()` explicitly at the earliest safe moment (CLAUDE.md §7c).

---

## 15. Unknowns / Assumptions

- `mbedtls_ssl_session` is an opaque mbedTLS struct; its size is mbedTLS-version-dependent.
- `session_resumption_enabled` is a field in `TlsConfig` (passed to `init()`).
