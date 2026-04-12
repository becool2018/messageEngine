// Copyright 2026 Don Jessup
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file TlsSessionStore.cpp
 * @brief TlsSessionStore: caller-owned TLS session snapshot for resumption.
 *
 * Power of 10 Rule 3: no dynamic allocation after init — mbedtls_ssl_session
 * is a fixed-size member of the struct; all mbedTLS calls operate in-place.
 *
 * CLAUDE.md §7c: use mbedtls_platform_zeroize (not memset) for session
 * material — the compiler may elide memset on dead storage (CWE-14).
 *
 * REQ-6.3.10 / HAZ-021 / CWE-362: all accesses to m_session are serialised
 * by m_mutex to prevent a concurrent zeroize() / try_load() race that could
 * cause ssl_set_session() to operate on partially-zeroed session material,
 * producing a TLS handshake with corrupt session keys.
 *
 * Implements: REQ-6.3.4, REQ-6.3.10
 */
// Implements: REQ-6.3.4, REQ-6.3.10

#include "platform/TlsSessionStore.hpp"
#include "core/Assert.hpp"

#include <mbedtls/ssl.h>
#include <mbedtls/platform_util.h>  // mbedtls_platform_zeroize — §7c (CWE-14)
#include <pthread.h>                 // pthread_mutex_* — REQ-6.3.10

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::TlsSessionStore()
// ─────────────────────────────────────────────────────────────────────────────

TlsSessionStore::TlsSessionStore()
    : session_valid(false), m_session{}, m_mutex{}  // §7b: init at declaration
{
    // Initialise POSIX mutex — REQ-6.3.10 (HAZ-021, CWE-362).
    // Power of 10 Rule 2: no loop — single fixed-count operation.
    const int rc = pthread_mutex_init(&m_mutex, nullptr);
    // Power of 10 Rule 5: ≥2 assertions per function.
    // Assert 1: mutex initialised successfully — fatal if POSIX call fails.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);
    mbedtls_ssl_session_init(&m_session);
    // Assert 2: invariant — session_valid must be false after construction.
    NEVER_COMPILED_OUT_ASSERT(!session_valid.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::~TlsSessionStore()
// ─────────────────────────────────────────────────────────────────────────────

TlsSessionStore::~TlsSessionStore()
{
    // §7c safety net: zeroize() acquires the mutex and clears session material
    // so it does not remain in freed stack or heap memory
    // (SECURITY_ASSUMPTIONS.md §13).
    zeroize();
    // Destroy the mutex after the last locked operation completes.
    const int rc = pthread_mutex_destroy(&m_mutex);
    // Power of 10 Rule 5: ≥2 assertions per function.
    // Assert 1: mutex destroyed successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);
    // Assert 2: session_valid must be false after zeroize() (re-check).
    NEVER_COMPILED_OUT_ASSERT(!session_valid.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::zeroize_unlocked()  — private helper, no mutex
// ─────────────────────────────────────────────────────────────────────────────

/// Perform the actual zeroize sequence without acquiring the mutex.
/// MUST only be called while the caller holds m_mutex.
void TlsSessionStore::zeroize_unlocked()
{
    // Safety-critical (SC): HAZ-012, HAZ-017
    // Order matters:
    //   1. session_free() — releases any internal allocations (ticket data,
    //      cert chain) that mbedTLS may have attached during ssl_get_session.
    //   2. platform_zeroize() — overwrites all raw bytes including the now-
    //      freed-but-still-readable internal pointers and key bytes; the
    //      compiler barrier prevents elision (CWE-14, CLAUDE.md §7c).
    //   3. session_init() — re-initialises the struct to a known-safe zero
    //      state so the object can be reused or safely destructed a second time.

    mbedtls_ssl_session_free(&m_session);
    // MISRA C++:2023 Rule 5.2.4 permission: static_cast<void*> required to
    // convert pointer-to-object to pointer-to-byte for mbedtls_platform_zeroize()
    // which requires a void* argument.  No safer cast suffices.
    mbedtls_platform_zeroize(static_cast<void*>(&m_session), sizeof(m_session));
    mbedtls_ssl_session_init(&m_session);
    session_valid.store(false);

    // Power of 10 Rule 5: two post-condition assertions.
    // Assert 1 — flag is cleared (negation form).
    NEVER_COMPILED_OUT_ASSERT(!session_valid.load());
    // Assert 2 — flag is cleared (equality form): double-checks atomic
    // visibility; both assertions would fail if session_valid.store(false)
    // were accidentally removed.
    NEVER_COMPILED_OUT_ASSERT(session_valid.load() == false);
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::zeroize()
// Safety-critical (SC): HAZ-012, HAZ-017, HAZ-021
// ─────────────────────────────────────────────────────────────────────────────

void TlsSessionStore::zeroize()
{
    // REQ-6.3.10 / HAZ-021: acquire mutex before any access to m_session.
    int rc = pthread_mutex_lock(&m_mutex);
    // Power of 10 Rule 5: ≥2 assertions per function.
    // Assert 1: mutex locked successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);

    zeroize_unlocked();  // clears m_session and sets session_valid=false

    rc = pthread_mutex_unlock(&m_mutex);
    // Assert 2: mutex unlocked successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::try_save()
// Safety-critical (SC): HAZ-017, HAZ-021
// ─────────────────────────────────────────────────────────────────────────────

int TlsSessionStore::try_save(const mbedtls_ssl_context* ssl, IMbedtlsOps& ops)
{
    // Safety-critical (SC): HAZ-017, HAZ-021 (REQ-6.3.10)
    // Power of 10 Rule 5: ≥2 assertions per function.
    // Assert 1: pre-condition — ssl context must be non-null.
    NEVER_COMPILED_OUT_ASSERT(ssl != nullptr);

    // REQ-6.3.10: acquire mutex for the full save operation (zeroize + get_session
    // + set_valid) as an atomic unit to prevent a concurrent zeroize() from
    // clearing m_session between the ssl_get_session call and session_valid.store.
    int rc = pthread_mutex_lock(&m_mutex);
    // Assert 2: mutex locked successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);

    // Release any stale session before overwriting with the fresh snapshot.
    zeroize_unlocked();

    const int ret = ops.ssl_get_session(ssl, &m_session);
    if (ret == 0) {
        session_valid.store(true);
    }
    // else: session_valid remains false (set by zeroize_unlocked above).

    rc = pthread_mutex_unlock(&m_mutex);
    // Assert 3 (additional): mutex unlocked successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);

    return ret;  // 0 = saved; non-zero = ssl_get_session failure code
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::try_load()
// Safety-critical (SC): HAZ-017, HAZ-021
// ─────────────────────────────────────────────────────────────────────────────

bool TlsSessionStore::try_load(mbedtls_ssl_context* ssl, IMbedtlsOps& ops)
{
    // Safety-critical (SC): HAZ-017, HAZ-021 (REQ-6.3.10)
    // Power of 10 Rule 5: ≥2 assertions per function.
    // Assert 1: pre-condition — ssl context must be non-null.
    NEVER_COMPILED_OUT_ASSERT(ssl != nullptr);

    // REQ-6.3.10: acquire mutex and re-check session_valid under the lock to
    // close the TOCTOU window between has_resumable_session() and try_load().
    // If another thread called zeroize() between those two calls, session_valid
    // is now false and we must not pass the freed m_session to ssl_set_session.
    int rc = pthread_mutex_lock(&m_mutex);
    // Assert 2: mutex locked successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);

    bool loaded = false;
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    if (session_valid.load()) {
        const int ret = ops.ssl_set_session(ssl, &m_session);
        loaded = (ret == 0);
    }
    // else: session_valid became false under the lock (TOCTOU race, rare).
    // full TLS handshake will proceed; caller logs if needed.
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

    rc = pthread_mutex_unlock(&m_mutex);
    // Assert 3 (additional): mutex unlocked successfully.
    NEVER_COMPILED_OUT_ASSERT(rc == 0);

    return loaded;
}
