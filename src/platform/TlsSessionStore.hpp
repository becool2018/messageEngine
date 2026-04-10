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
 *   client2.init(cfg);                 // try_load_client_session() reachable now
 *   ...
 *   store.zeroize();                   // caller's responsibility (CLAUDE.md §7c)
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
 * Implements: REQ-6.3.4
 * Safety-critical (SC): HAZ-012, HAZ-017 (zeroize() method)
 * NSC: construction, valid flag read, set_session_store()
 */
// Implements: REQ-6.3.4

#ifndef PLATFORM_TLS_SESSION_STORE_HPP
#define PLATFORM_TLS_SESSION_STORE_HPP

#include <atomic>          // std::atomic<bool> — CLAUDE.md §1d carve-out
#include <mbedtls/ssl.h>

/**
 * @struct TlsSessionStore
 * @brief Caller-owned container for a single TLS session snapshot.
 *
 * Non-copyable and non-movable: mbedtls_ssl_session contains internal
 * pointers and must not be bitwise-copied (mbedTLS API constraint).
 *
 * Lifecycle:
 *   1. Default-construct: session zeroed, session_valid = false.
 *   2. TlsTcpBackend::try_save_client_session() writes session data and
 *      sets session_valid = true on success.
 *   3. TlsTcpBackend::try_load_client_session() reads session_valid and
 *      calls mbedtls_ssl_set_session() on the next init().
 *   4. Caller calls zeroize() when the session is no longer needed.
 *   5. Destructor calls zeroize() as a safety net.
 */
struct TlsSessionStore {
    /// Embedded TLS session — fixed size, no heap (Power of 10 Rule 3).
    mbedtls_ssl_session session;

    /// True when session contains usable, unspoiled TLS session material.
    /// Set by try_save_client_session() on success; cleared by zeroize().
    /// §7b: initialized at declaration via constructor.
    ///
    /// std::atomic<bool> — CLAUDE.md §1d carve-out (F-6):
    /// Concurrent store.zeroize() (app thread) and try_load_client_session()
    /// (backend thread) would be a data race on a plain bool (CWE-362).
    /// Using std::atomic<bool> eliminates the race without requiring a mutex.
    /// Callers must still avoid concurrent zeroize() + backend I/O at the
    /// higher level (session data is not atomically swapped); atomic<bool>
    /// guards the flag only.  Thread-safety contract: the caller must not
    /// call zeroize() concurrently with any backend operation that
    /// dereferences m_session_store_ptr.
    std::atomic<bool> session_valid;

    /// Default constructor: zero-inits session struct, sets session_valid=false.
    TlsSessionStore();

    /// Destructor: calls zeroize() to clear session material (§7c safety net).
    ~TlsSessionStore();

    /// Zeroize TLS session material and reset to invalid state.
    ///
    /// Calls mbedtls_ssl_session_free(), then mbedtls_platform_zeroize() on the
    /// entire session struct, then mbedtls_ssl_session_init() to return it to a
    /// clean state.  Uses mbedtls_platform_zeroize (not memset) to prevent
    /// compiler elision of the zeroing (CWE-14, CLAUDE.md §7c).
    ///
    /// Safety-critical (SC): HAZ-012, HAZ-017
    /// Callers must invoke this method when session material is no longer needed.
    void zeroize();

    // Non-copyable — mbedtls_ssl_session bitwise copy is forbidden by mbedTLS API.
    // Power of 10 Rule 9: no function pointer declarations; vtable not used here.
    TlsSessionStore(const TlsSessionStore&)            = delete;
    TlsSessionStore& operator=(const TlsSessionStore&) = delete;
    TlsSessionStore(TlsSessionStore&&)                  = delete;
    TlsSessionStore& operator=(TlsSessionStore&&)       = delete;
};

#endif // PLATFORM_TLS_SESSION_STORE_HPP
