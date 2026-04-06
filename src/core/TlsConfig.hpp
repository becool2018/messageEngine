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
 * @file TlsConfig.hpp
 * @brief TLS configuration struct for use with TlsTcpBackend.
 *
 * Plain POD configuration — no platform headers, no POSIX dependencies.
 * Lives in src/core/ so that TransportConfig (in ChannelConfig.hpp) can
 * embed it without creating a core → platform layering violation.
 *
 * When tls_enabled is false, the associated TlsTcpBackend operates in
 * plaintext mode and behaves identically to TcpBackend.
 *
 * Rules applied:
 *   - Power of 10 Rule 3: fixed-size char arrays; no dynamic allocation.
 *   - Power of 10 Rule 8: no macros for control flow or constants.
 *   - MISRA C++:2023: plain struct; no STL, no templates, no exceptions.
 *   - F-Prime style: inline default function; no global mutable state.
 *
 * Implements: REQ-6.3.4
 */
// Implements: REQ-6.3.4

#ifndef CORE_TLS_CONFIG_HPP
#define CORE_TLS_CONFIG_HPP

#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Capacity constant
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum length of a file-path string, including the NUL terminator.
static const uint32_t TLS_PATH_MAX = 256U;

// ─────────────────────────────────────────────────────────────────────────────
// TlsRole — endpoint role for the TLS handshake
// ─────────────────────────────────────────────────────────────────────────────

enum class TlsRole : uint8_t {
    CLIENT = 0U,  ///< Initiates the TLS handshake (mbedTLS IS_CLIENT)
    SERVER = 1U   ///< Accepts the TLS handshake  (mbedTLS IS_SERVER)
};

// ─────────────────────────────────────────────────────────────────────────────
// TlsConfig — all TLS parameters for one transport endpoint
// ─────────────────────────────────────────────────────────────────────────────

struct TlsConfig {
    /// Master switch.
    /// false (default) = plaintext TCP; TlsTcpBackend acts like TcpBackend.
    /// true            = TLS 1.2+ encryption and (optionally) peer verification.
    bool tls_enabled;

    /// Endpoint role — must match the peer's opposite role.
    TlsRole role;

    /// Path to the PEM-encoded certificate file for this endpoint.
    /// Required when tls_enabled is true.
    /// May be a certificate chain (own cert + intermediates in one PEM file).
    char cert_file[TLS_PATH_MAX];

    /// Path to the PEM-encoded private key file matching cert_file.
    /// Required when tls_enabled is true.
    char key_file[TLS_PATH_MAX];

    /// Path to the PEM-encoded CA (Certificate Authority) certificate used to
    /// verify the peer's certificate chain.
    /// Required when verify_peer is true.
    char ca_file[TLS_PATH_MAX];

    /// true  = verify the peer's certificate (MBEDTLS_SSL_VERIFY_REQUIRED).
    /// false = skip peer verification (MBEDTLS_SSL_VERIFY_NONE).
    ///         Only disable for testing — never in production.
    bool verify_peer;

    /// Expected server hostname for TLS SNI and certificate hostname verification.
    /// Used by TLS clients only (ignored by servers).
    /// Required by mbedTLS 4.0+ when verify_peer is true — set to the CN or SAN
    /// that the server certificate is expected to present (e.g. "example.com").
    /// Leave empty ("") only when verify_peer is false (no cert check performed).
    char peer_hostname[TLS_PATH_MAX];

    /// Path to the PEM-encoded CRL (Certificate Revocation List) file.
    /// Optional. When non-empty and verify_peer is true, the CA chain
    /// verification is performed against this CRL to reject revoked certs.
    /// Leave empty ("") to skip CRL checking (e.g. in testing).
    char crl_file[TLS_PATH_MAX];

    /// Enable TLS session ticket resumption (REQ-6.3.4 extension point).
    /// Client: saves the session after a successful handshake and presents it
    ///         on reconnect to skip the full handshake (RFC 5077 / TLS 1.3 PSK).
    /// Server: enables mbedtls_ssl_conf_session_tickets() so clients may resume.
    /// Default false — backward-compatible with all existing callers.
    /// Power of 10 Rule 3: session state is fixed-size (mbedtls_ssl_session).
    bool session_resumption_enabled;

    /// Server-side session ticket lifetime in seconds (RFC 5077 §3.3).
    /// Passed to mbedtls_ssl_conf_session_tickets() on the server only.
    /// Ignored when session_resumption_enabled is false or role is CLIENT.
    /// Default 86400 (24 h).  Power of 10 Rule 3: no dynamic allocation.
    uint32_t session_ticket_lifetime_s;
};

// ─────────────────────────────────────────────────────────────────────────────
// Default initializer (inline; no global state)
// ─────────────────────────────────────────────────────────────────────────────

/// Populate @p cfg with safe, plaintext defaults.
/// tls_enabled is false so the transport operates without encryption unless
/// the caller explicitly sets tls_enabled = true and provides cert/key paths.
inline void tls_config_default(TlsConfig& cfg)
{
    cfg.tls_enabled = false;
    cfg.role        = TlsRole::CLIENT;
    (void)memset(cfg.cert_file,     0, TLS_PATH_MAX);
    (void)memset(cfg.key_file,      0, TLS_PATH_MAX);
    (void)memset(cfg.ca_file,       0, TLS_PATH_MAX);
    (void)memset(cfg.peer_hostname, 0, TLS_PATH_MAX);
    (void)memset(cfg.crl_file,      0, TLS_PATH_MAX);
    cfg.verify_peer                = true;   // Secure default: always verify when TLS is on
    cfg.session_resumption_enabled = false;  // Off by default — backward-compatible
    cfg.session_ticket_lifetime_s  = 86400U; // 24 h default ticket lifetime
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_path_valid — NUL-termination check for TlsConfig path fields
// ─────────────────────────────────────────────────────────────────────────────

/// Return true if @p path contains at least one NUL byte within [0, TLS_PATH_MAX).
/// Prevents non-NUL-terminated strings from being passed to fopen() via mbedTLS,
/// which would cause a stack overread (CWE-120).  SECfix-5.
///
/// Callers in TlsTcpBackend and DtlsUdpBackend wrap this in
/// NEVER_COMPILED_OUT_ASSERT(tls_path_valid(...)) so the production assert macro
/// is applied at the point of use where Assert.hpp is already available.
///
/// Power of 10 Rule 2: bounded loop (TLS_PATH_MAX compile-time constant).
/// Power of 10 Rule 3: no dynamic allocation.
inline bool tls_path_valid(const char* const path)
{
    if (path == nullptr) { return false; }

    // Power of 10 Rule 2: bounded loop — compile-time constant upper bound.
    for (uint32_t i = 0U; i < TLS_PATH_MAX; ++i) {
        if (path[i] == '\0') {
            return true;
        }
    }
    return false;  // No NUL found within TLS_PATH_MAX bytes — invalid (CWE-120)
}

#endif // CORE_TLS_CONFIG_HPP
