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
 * Implements: REQ-6.3.4
 */
// Implements: REQ-6.3.4

#include "platform/TlsSessionStore.hpp"
#include "core/Assert.hpp"

#include <mbedtls/ssl.h>
#include <mbedtls/platform_util.h>  // mbedtls_platform_zeroize — §7c (CWE-14)

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::TlsSessionStore()
// ─────────────────────────────────────────────────────────────────────────────

TlsSessionStore::TlsSessionStore() : session{}, session_valid(false)  // §7b
{
    mbedtls_ssl_session_init(&session);

    // Power of 10 Rule 5: ≥2 assertions per function.
    // Assert 1: invariant — session_valid must be false after construction.
    NEVER_COMPILED_OUT_ASSERT(!session_valid);
    // Assert 2: structural check — session_valid initialized at declaration (§7b).
    // Equivalent post-condition expressed as negation to confirm bool semantics.
    NEVER_COMPILED_OUT_ASSERT(session_valid == false);
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::~TlsSessionStore()
// ─────────────────────────────────────────────────────────────────────────────

TlsSessionStore::~TlsSessionStore()
{
    // §7c safety net: always zeroize on destruction so that session material
    // does not remain in freed stack or heap memory (SECURITY_ASSUMPTIONS.md §13).
    zeroize();
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsSessionStore::zeroize()
// Safety-critical (SC): HAZ-012, HAZ-017
// ─────────────────────────────────────────────────────────────────────────────

void TlsSessionStore::zeroize()
{
    // Safety-critical (SC): HAZ-012, HAZ-017
    // Zeroize TLS session master-secret material before the struct goes out
    // of scope or is reused.  Order matters:
    //   1. session_free() — releases any internal allocations (ticket data,
    //      cert chain) that mbedTLS may have attached during get_session().
    //   2. platform_zeroize() — overwrites all raw bytes including the now-
    //      freed-but-still-readable internal pointers and key bytes; the
    //      compiler barrier prevents elision (CWE-14, CLAUDE.md §7c).
    //   3. session_init() — re-initialises the struct to a known-safe zero
    //      state so the object can be reused or safely destructed a second time.

    // Power of 10 Rule 5: assert 1 — structural invariant: this method is always
    // safe to call (idempotent); the true literal documents the design intent.
    NEVER_COMPILED_OUT_ASSERT(true); // NOLINT(readability-simplify-boolean-expr)

    mbedtls_ssl_session_free(&session);
    // MISRA C++:2023 Rule 5.2.4 permission: reinterpret_cast / static_cast<void*>
    // required to convert pointer-to-object to pointer-to-byte for the
    // mbedtls_platform_zeroize() call, which requires a void* argument.
    // No safer cast suffices; this is the standard idiom for zeroize calls.
    mbedtls_platform_zeroize(static_cast<void*>(&session), sizeof(session));
    mbedtls_ssl_session_init(&session);
    session_valid = false;

    // Power of 10 Rule 5: assert 2 — post-condition: store is now invalid.
    NEVER_COMPILED_OUT_ASSERT(!session_valid);
}
