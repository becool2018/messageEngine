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
    (void)memset(cfg.cert_file, 0, TLS_PATH_MAX);
    (void)memset(cfg.key_file,  0, TLS_PATH_MAX);
    (void)memset(cfg.ca_file,   0, TLS_PATH_MAX);
    cfg.verify_peer = true;   // Secure default: always verify when TLS is on
}

#endif // CORE_TLS_CONFIG_HPP
