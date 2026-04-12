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
 * @file TlsSessionStore.hpp
 * @brief Caller-owned value type that holds a TLS session snapshot for
 *        client-side session resumption across close() → init() cycles.
 *
 * Motivation (Class B coverage gap):
 *   The original TlsTcpBackend design stored the TLS session in a private
 *   member (m_saved_session / m_session_saved).  Because close() zeroed and
 *   re-initialised the session in the same function scope, m_session_saved was
 *   always false when the next init() ran.  This made try_load_client_session()
 *   structurally unreachable (COVERAGE_CEILINGS.md round 6 ceiling note).
 *   Moving session ownership to the caller breaks the lifecycle coupling:
 *   the caller controls the store's lifetime and can present the same store
 *   to a second TlsTcpBackend instance after close().
 *
 * Usage:
 *   TlsSessionStore store;           // stack or member; caller owns it
 *   TlsTcpBackend client;
 *   client.set_session_store(&store); // inject before init()
 *   client.init(cfg);                 // first connection; saves session on success
 *   ...
 *   client.close();                   // store.session_valid remains true
 *   TlsTcpBackend client2;
 *   client2.set_session_store(&store); // same store — session_valid==true
 *   client2.init(cfg);                 // try_load reachable now
 *   ...
 *   store.zeroize();                   // caller's responsibility (CLAUDE.md §7c)
 *
 * Thread safety (REQ-6.3.10 / HAZ-021 / CWE-362):
 *   All accesses to the mbedtls_ssl_session struct (m_session) are serialised
 *   by an internal POSIX mutex.  zeroize(), try_save(), and try_load() each
 *   lock the mutex for their entire duration.  The std::atomic<bool>
 *   session_valid flag is also re-checked under the lock in try_load() to
 *   close the TOCTOU window between has_resumable_session() and try_load().
 *
 * Security contract (SECURITY_ASSUMPTIONS.md §13):
 *   The caller MUST call store.zeroize() when the session material is no longer
 *   needed.  TlsSessionStore::~TlsSessionStore() calls zeroize() as a safety net,
 *   but relying on the destructor alone means key material survives until the
 *   object goes out of scope rather than being cleared at the earliest safe moment.
 *
 * Power of 10 Rule 3: no heap allocation — mbedtls_ssl_session is embedded
 * directly in the struct as a fixed-size value member.
 *
 * Implements: REQ-6.3.4, REQ-6.3.10
 * Safety-critical (SC): HAZ-012, HAZ-017, HAZ-021 (zeroize, try_save, try_load)
 * NSC: construction, valid flag read, set_session_store()
 */
// Implements: REQ-6.3.4, REQ-6.3.10

#ifndef PLATFORM_TLS_SESSION_STORE_HPP
#define PLATFORM_TLS_SESSION_STORE_HPP

#include <atomic>          // std::atomic<bool> — CLAUDE.md §1d carve-out
#include <mbedtls/ssl.h>
#include <pthread.h>       // POSIX mutex — REQ-6.3.10 (HAZ-021, CWE-362)

#include "platform/IMbedtlsOps.hpp"  // try_save / try_load parameter type

/**
 * @struct TlsSessionStore
 * @brief Caller-owned container for a single TLS session snapshot.
 *
 * Non-copyable and non-movable: mbedtls_ssl_session contains internal
 * pointers and must not be bitwise-copied (mbedTLS API constraint).
 *
 * Lifecycle:
 *   1. Default-construct: session zeroed, session_valid = false, mutex init.
 *   2. try_save() — locks mutex, zeroizes, calls ssl_get_session, sets
 *      session_valid = true on success (REQ-6.3.10).
 *   3. try_load() — locks mutex, re-checks session_valid, calls
 *      ssl_set_session if valid; returns bool (REQ-6.3.10).
 *   4. Caller calls zeroize() when the session is no longer needed.
 *   5. Destructor calls zeroize() as a safety net, then destroys the mutex.
 */
struct TlsSessionStore {
    /// True when the store contains usable, unspoiled TLS session material.
    /// Set by try_save() on success; cleared by zeroize() (under mutex).
    /// §7b: initialized at declaration via constructor.
    ///
    /// std::atomic<bool> — CLAUDE.md §1d carve-out (F-6):
    /// Allows lock-free fast-path flag reads (has_resumable_session()) while
    /// the session struct itself is protected by m_mutex.
    /// The TOCTOU window between flag check and struct access is closed inside
    /// try_load() by re-checking session_valid under the mutex lock.
    std::atomic<bool> session_valid;

    /// Default constructor: zero-inits session struct, inits mutex,
    /// sets session_valid=false.
    TlsSessionStore();

    /// Destructor: calls zeroize() to clear session material (§7c safety net),
    /// then destroys the POSIX mutex.
    ~TlsSessionStore();

    /// Zeroize TLS session material and reset to invalid state (mutex-protected).
    ///
    /// Acquires m_mutex, calls the internal zeroize sequence (session_free,
    /// platform_zeroize, session_init, session_valid=false), then releases the
    /// mutex.  Uses mbedtls_platform_zeroize (not memset) to prevent compiler
    /// elision of the zeroing (CWE-14, CLAUDE.md §7c).
    ///
    /// Safety-critical (SC): HAZ-012, HAZ-017, HAZ-021
    void zeroize();

    /// Save the current TLS session from @p ssl into this store (mutex-protected).
    ///
    /// Acquires m_mutex; zeroizes any prior session; calls ops.ssl_get_session();
    /// sets session_valid=true on success.  Returns the ssl_get_session return
    /// code (0 = saved; non-zero = failure, session_valid remains false).
    ///
    /// Safety-critical (SC): HAZ-017, HAZ-021 (REQ-6.3.10)
    // Safety-critical (SC): HAZ-017, HAZ-021
    int try_save(const mbedtls_ssl_context* ssl, IMbedtlsOps& ops);

    /// Load the stored session into @p ssl before the TLS handshake (mutex-protected).
    ///
    /// Acquires m_mutex; re-checks session_valid (TOCTOU prevention); calls
    /// ops.ssl_set_session() when valid.  Returns true if the session was
    /// loaded successfully; false if the session was not valid under the lock
    /// or if ssl_set_session failed.  Failure is non-fatal — the caller
    /// proceeds with a full TLS handshake.
    ///
    /// Safety-critical (SC): HAZ-017, HAZ-021 (REQ-6.3.10)
    // Safety-critical (SC): HAZ-017, HAZ-021
    bool try_load(mbedtls_ssl_context* ssl, IMbedtlsOps& ops);

    // Non-copyable — mbedtls_ssl_session bitwise copy is forbidden by mbedTLS API.
    // Power of 10 Rule 9: no function pointer declarations; vtable not used here.
    TlsSessionStore(const TlsSessionStore&)            = delete;
    TlsSessionStore& operator=(const TlsSessionStore&) = delete;
    TlsSessionStore(TlsSessionStore&&)                  = delete;
    TlsSessionStore& operator=(TlsSessionStore&&)       = delete;

private:
    /// Embedded TLS session — fixed size, no heap (Power of 10 Rule 3).
    /// Protected by m_mutex; accessed only via try_save(), try_load(), and
    /// the internal zeroize_unlocked() helper.
    mbedtls_ssl_session m_session;

    /// POSIX mutex protecting m_session against concurrent access (REQ-6.3.10,
    /// HAZ-021, CWE-362).  Locked for the full duration of zeroize(),
    /// try_save(), and try_load().
    pthread_mutex_t m_mutex;

    /// Internal zeroize without acquiring the mutex (called by zeroize(),
    /// try_save(), and ~TlsSessionStore() while the caller holds the lock or
    /// in single-threaded destruction context).
    void zeroize_unlocked();
};

#endif // PLATFORM_TLS_SESSION_STORE_HPP
