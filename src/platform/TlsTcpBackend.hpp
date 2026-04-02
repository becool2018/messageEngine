/**
 * @file TlsTcpBackend.hpp
 * @brief TLS-capable TCP transport backend implementing TransportInterface.
 *
 * Drop-in replacement for TcpBackend that adds optional mbedTLS encryption.
 * When config.tls.tls_enabled is false, operates in plaintext mode and
 * is functionally identical to TcpBackend (REQ-6.3.4: swappable without
 * changing higher layers).
 *
 * When config.tls.tls_enabled is true:
 *   - Loads the PEM certificate (cert_file), private key (key_file), and
 *     optional CA certificate (ca_file) during init().
 *   - Performs a TLS 1.2+ handshake on every accepted or initiated connection.
 *   - All send/receive operations pass through mbedTLS record layer.
 *   - Message framing (4-byte big-endian length prefix) is preserved.
 *
 * Encryption library: mbedTLS 4.0 (PSA Crypto backend).
 *   RNG: PSA Crypto internal DRBG (psa_crypto_init() called in init()).
 *   No legacy CTR-DRBG or entropy context required.
 *
 * Power of 10 Rule 3 note:
 *   mbedTLS internally heap-allocates during the TLS handshake (record buffers,
 *   certificate parsing). This occurs only during init()/accept() — not on the
 *   send/receive critical path. This is documented as an init-phase deviation.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, ≥2 assertions per function,
 *     all return values checked, fixed stack buffers.
 *   - MISRA C++:2023: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result return codes; Logger::log() for events.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6,
 *             REQ-6.3.4, REQ-7.1.1
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.3.4, REQ-7.1.1

#ifndef PLATFORM_TLS_TCP_BACKEND_HPP
#define PLATFORM_TLS_TCP_BACKEND_HPP

#include <cstdint>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>

#include "core/Assert.hpp"
#include "core/TransportInterface.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"
#include "core/RingBuffer.hpp"
#include "platform/ImpairmentEngine.hpp"

class ISocketOps;   // forward declaration — see platform/ISocketOps.hpp

// ─────────────────────────────────────────────────────────────────────────────
// TlsTcpBackend
// ─────────────────────────────────────────────────────────────────────────────

class TlsTcpBackend : public TransportInterface {
public:
    /// Default constructor — uses the process-wide SocketOpsImpl singleton.
    /// Production code always calls this form.
    TlsTcpBackend();

    /// Injection constructor — accepts any ISocketOps implementation.
    /// Used by tests to inject a mock and exercise error paths that cannot
    /// be triggered in a loopback environment.
    /// @p sock_ops must outlive this TlsTcpBackend instance.
    explicit TlsTcpBackend(ISocketOps& sock_ops);

    ~TlsTcpBackend() override;

    // ── TransportInterface implementation ────────────────────────────────────
    Result init(const TransportConfig& config) override;
    // Safety-critical (SC): HAZ-005, HAZ-006 — verified to M5
    Result send_message(const MessageEnvelope& envelope) override;
    // Safety-critical (SC): HAZ-004, HAZ-005 — verified to M5
    Result receive_message(MessageEnvelope& envelope, uint32_t timeout_ms) override;
    void   close() override;
    bool   is_open() const override;

private:
    // ── mbedTLS contexts (fixed static allocation — Power of 10 Rule 3) ─────
    mbedtls_ssl_config  m_ssl_conf;                        ///< Shared TLS config
    mbedtls_x509_crt    m_cert;                            ///< Own certificate chain
    mbedtls_x509_crt    m_ca_cert;                         ///< CA certificate for verification
    mbedtls_pk_context  m_pkey;                            ///< Own private key
    mbedtls_net_context m_listen_net;                      ///< Server listen socket
    mbedtls_net_context m_client_net[MAX_TCP_CONNECTIONS]; ///< Per-client net contexts
    mbedtls_ssl_context m_ssl[MAX_TCP_CONNECTIONS];        ///< Per-client TLS sessions

    // ── Injected socket operations interface ─────────────────────────────────
    ISocketOps*       m_sock_ops;                          ///< Non-owning; never null after ctor

    // ── Transport state ──────────────────────────────────────────────────────
    uint32_t          m_client_count;
    uint8_t           m_wire_buf[SOCKET_RECV_BUF_BYTES];
    TransportConfig   m_cfg;
    ImpairmentEngine  m_impairment;
    RingBuffer        m_recv_queue;
    bool              m_open;
    bool              m_is_server;
    bool              m_tls_enabled;   ///< From config.tls.tls_enabled

    // ── Private helpers ──────────────────────────────────────────────────────

    /// Load certificates and key; configure mbedTLS shared config object.
    /// Called once during init() when tls_enabled is true.
    Result setup_tls_config(const TlsConfig& tls_cfg);

    /// Bind listen socket and start accepting (server mode).
    Result bind_and_listen(const char* ip, uint16_t port);

    /// Establish outbound connection and optionally perform TLS handshake.
    Result connect_to_server();

    /// Perform TLS setup (set_block, ssl_setup, set_hostname, BIO, handshake)
    /// for the client socket at slot 0. Called by connect_to_server() when
    /// tls_enabled is true. Extracted to reduce connect_to_server() CC.
    Result tls_connect_handshake();

    /// Accept one pending connection and optionally perform TLS handshake.
    /// Does nothing if no pending connection or table is full.
    Result accept_and_handshake();

    /// Receive and deserialize one frame from client at index @p idx.
    Result recv_from_client(uint32_t idx, uint32_t timeout_ms);

    /// Serialize + send @p buf/@p len to all connected clients.
    void send_to_all_clients(const uint8_t* buf, uint32_t len);

    /// Close and remove client at array index @p idx; compact the table.
    void remove_client(uint32_t idx);

    /// Flush impairment-delayed messages out to connected clients.
    void flush_delayed_to_clients(uint64_t now_us);

    /// Flush impairment-delayed inbound messages into the receive queue.
    void flush_delayed_to_queue(uint64_t now_us);

    /// Accept new clients (server) and drain one frame per active client.
    void poll_clients_once(uint32_t timeout_ms);

    // ── TLS-aware framing helpers ─────────────────────────────────────────────

    /// Send a 4-byte length-prefixed frame via TLS (tls_enabled) or raw TCP.
    bool tls_send_frame(uint32_t idx, const uint8_t* buf, uint32_t len,
                        uint32_t timeout_ms);

    /// Receive a 4-byte length-prefixed frame via TLS (tls_enabled) or raw TCP.
    bool tls_recv_frame(uint32_t idx, uint8_t* buf, uint32_t buf_cap,
                        uint32_t timeout_ms, uint32_t* out_len);

    // ── CC-reduction helper ───────────────────────────────────────────────────

    /// Read exactly @p payload_len bytes from m_ssl[idx] into @p buf, handling
    /// MBEDTLS_ERR_SSL_WANT_READ continuations. Sets *out_len on success.
    /// Extracted from tls_recv_frame() to reduce its CC.
    bool tls_read_payload(uint32_t idx, uint8_t* buf,
                          uint32_t payload_len, uint32_t* out_len);
};

#endif // PLATFORM_TLS_TCP_BACKEND_HPP
