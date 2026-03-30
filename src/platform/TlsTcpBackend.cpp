/**
 * @file TlsTcpBackend.cpp
 * @brief Implementation of TlsTcpBackend: TLS-capable TCP transport.
 *
 * Uses mbedTLS 4.0 with PSA Crypto backend for TLS 1.2/1.3 encryption.
 * When config.tls.tls_enabled is false, falls back to plaintext TCP using
 * the raw fd inside mbedtls_net_context — no TLS overhead.
 *
 * Power of 10 Rule 3 deviation (init-phase):
 *   mbedTLS heap-allocates during psa_crypto_init(), certificate parsing,
 *   and TLS handshake. These occur exclusively in init() / accept_and_handshake().
 *   The send/receive critical path (after handshake completes) does not
 *   allocate. Documented per CLAUDE.md §2 Rule 3 exception.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6,
 *             REQ-6.3.4, REQ-7.1.1
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.3.4, REQ-7.1.1

#include "platform/TlsTcpBackend.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include "core/Serializer.hpp"
#include "core/Timestamp.hpp"
#include "platform/SocketUtils.hpp"

#include <mbedtls/error.h>
#include <psa/crypto.h>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — convert port uint16_t → "NNNNN" string
// ─────────────────────────────────────────────────────────────────────────────

/// mbedtls_net_bind / mbedtls_net_connect take port as a decimal string.
/// Write the decimal representation of @p port into @p buf (capacity 6 bytes).
static void port_to_str(uint16_t port, char* buf)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    // Maximum uint16 = 65535 — 5 digits + NUL = 6 bytes.
    // Power of 10 Rule 2: bounded loop (at most 5 iterations).
    static const uint32_t PORT_BUF_LEN = 6U;
    uint32_t val = static_cast<uint32_t>(port);
    uint32_t pos = PORT_BUF_LEN - 1U;
    buf[pos] = '\0';
    if (val == 0U) {
        --pos;
        buf[pos] = '0';
    } else {
        for (uint32_t i = 0U; i < 5U && val > 0U; ++i) {
            --pos;
            buf[pos] = static_cast<char>('0' + static_cast<char>(val % 10U));
            val /= 10U;
        }
    }
    // Shift result to buf[0]
    uint32_t src = pos;
    uint32_t dst = 0U;
    for (; buf[src] != '\0'; ++src, ++dst) {
        buf[dst] = buf[src];
    }
    buf[dst] = '\0';
    NEVER_COMPILED_OUT_ASSERT(buf[0] != '\0');
}

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — log mbedTLS error code
// ─────────────────────────────────────────────────────────────────────────────

static void log_mbedtls_err(const char* tag, const char* func, int ret)
{
    NEVER_COMPILED_OUT_ASSERT(tag != nullptr);
    NEVER_COMPILED_OUT_ASSERT(func != nullptr);
    char err_buf[128];
    (void)memset(err_buf, 0, sizeof(err_buf));
    mbedtls_strerror(ret, err_buf, sizeof(err_buf) - 1U);
    Logger::log(Severity::WARNING_HI, tag, "%s failed: -0x%04X (%s)",
                func, static_cast<unsigned int>(-ret), err_buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

TlsTcpBackend::TlsTcpBackend()
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_listen_net{}, m_client_net{}, m_ssl{},
      m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_net_init(&m_listen_net);
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_init(&m_client_net[i]);
        mbedtls_ssl_init(&m_ssl[i]);
    }
}

TlsTcpBackend::~TlsTcpBackend()
{
    TlsTcpBackend::close();
    mbedtls_ssl_config_free(&m_ssl_conf);
    mbedtls_x509_crt_free(&m_cert);
    mbedtls_x509_crt_free(&m_ca_cert);
    mbedtls_pk_free(&m_pkey);
    mbedtls_net_free(&m_listen_net);
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_ssl_free(&m_ssl[i]);
        mbedtls_net_free(&m_client_net[i]);
    }
    mbedtls_psa_crypto_free();
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_tls_config() — load certs/keys; configure mbedTLS shared object
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::setup_tls_config(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');

    // Init PSA Crypto (mbedTLS 4.0: replaces CTR-DRBG / entropy setup).
    // Power of 10 Rule 3 deviation — init-phase heap allocation inside PSA.
    psa_status_t psa_ret = psa_crypto_init();
    if (psa_ret != PSA_SUCCESS) {
        Logger::log(Severity::FATAL, "TlsTcpBackend",
                    "psa_crypto_init failed: %d", static_cast<int>(psa_ret));
        return Result::ERR_IO;
    }

    int endpoint = (tls_cfg.role == TlsRole::SERVER)
                   ? MBEDTLS_SSL_IS_SERVER
                   : MBEDTLS_SSL_IS_CLIENT;

    int ret = mbedtls_ssl_config_defaults(&m_ssl_conf,
                                          endpoint,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_config_defaults", ret);
        return Result::ERR_IO;
    }

    // Set peer-verification mode
    int authmode = tls_cfg.verify_peer
                   ? MBEDTLS_SSL_VERIFY_REQUIRED
                   : MBEDTLS_SSL_VERIFY_NONE;
    mbedtls_ssl_conf_authmode(&m_ssl_conf, authmode);

    // Load CA certificate for peer verification (only when verify_peer is set)
    if (tls_cfg.verify_peer && tls_cfg.ca_file[0] != '\0') {
        ret = mbedtls_x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "x509_crt_parse_file (CA)", ret);
            return Result::ERR_IO;
        }
        mbedtls_ssl_conf_ca_chain(&m_ssl_conf, &m_ca_cert, nullptr);
    }

    // Load own certificate chain
    ret = mbedtls_x509_crt_parse_file(&m_cert, tls_cfg.cert_file);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "x509_crt_parse_file (cert)", ret);
        return Result::ERR_IO;
    }

    // Load own private key
    ret = mbedtls_pk_parse_keyfile(&m_pkey, tls_cfg.key_file, nullptr);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "pk_parse_keyfile", ret);
        return Result::ERR_IO;
    }

    // Associate cert + key with the shared SSL config
    ret = mbedtls_ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_conf_own_cert", ret);
        return Result::ERR_IO;
    }

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "TLS config ready: role=%s verify_peer=%d cert=%s",
                (tls_cfg.role == TlsRole::SERVER) ? "SERVER" : "CLIENT",
                static_cast<int>(tls_cfg.verify_peer),
                tls_cfg.cert_file);

    NEVER_COMPILED_OUT_ASSERT(psa_ret == PSA_SUCCESS);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// bind_and_listen() — server mode setup
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::bind_and_listen(const char* ip, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    NEVER_COMPILED_OUT_ASSERT(m_is_server);

    char port_str[6];
    port_to_str(port, port_str);

    int ret = mbedtls_net_bind(&m_listen_net, ip, port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_bind", ret);
        return Result::ERR_IO;
    }

    // Set non-blocking so accept() returns immediately when no client pending
    ret = mbedtls_net_set_nonblock(&m_listen_net);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_set_nonblock (listen)", ret);
        mbedtls_net_free(&m_listen_net);
        return Result::ERR_IO;
    }

    m_open = true;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Server listening on %s:%s (TLS=%s)", ip, port_str,
                m_tls_enabled ? "ON" : "OFF");

    NEVER_COMPILED_OUT_ASSERT(m_listen_net.fd >= 0);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// connect_to_server() — client mode
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::connect_to_server()
{
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_client_net[0U].fd < 0);

    char port_str[6];
    port_to_str(m_cfg.peer_port, port_str);

    int ret = mbedtls_net_connect(&m_client_net[0U], m_cfg.peer_ip,
                                  port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_connect", ret);
        return Result::ERR_IO;
    }

    if (m_tls_enabled) {
        // mbedtls_net_connect returns a blocking socket by default, but
        // explicitly set blocking mode to be safe before the TLS handshake.
        ret = mbedtls_net_set_block(&m_client_net[0U]);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "net_set_block (client connect)", ret);
            mbedtls_net_free(&m_client_net[0U]);
            return Result::ERR_IO;
        }

        ret = mbedtls_ssl_setup(&m_ssl[0U], &m_ssl_conf);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_setup (client)", ret);
            mbedtls_net_free(&m_client_net[0U]);
            return Result::ERR_IO;
        }
        mbedtls_ssl_set_bio(&m_ssl[0U], &m_client_net[0U],
                            mbedtls_net_send, mbedtls_net_recv, nullptr);

        // TLS handshake (Power of 10 Rule 3 deviation: init-phase heap alloc)
        ret = mbedtls_ssl_handshake(&m_ssl[0U]);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_handshake (client)", ret);
            mbedtls_ssl_free(&m_ssl[0U]);
            mbedtls_ssl_init(&m_ssl[0U]);
            mbedtls_net_free(&m_client_net[0U]);
            mbedtls_net_init(&m_client_net[0U]);
            return Result::ERR_IO;
        }
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "TLS handshake complete (client): cipher=%s",
                    mbedtls_ssl_get_ciphersuite(&m_ssl[0U]));
    }

    m_client_count = 1U;
    m_open         = true;

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Connected to %s:%s (TLS=%s)",
                m_cfg.peer_ip, port_str, m_tls_enabled ? "ON" : "OFF");

    NEVER_COMPILED_OUT_ASSERT(m_client_net[0U].fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(m_client_count == 1U);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// accept_and_handshake() — server mode, accept one pending connection
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::accept_and_handshake()
{
    NEVER_COMPILED_OUT_ASSERT(m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_listen_net.fd >= 0);

    if (m_client_count >= MAX_TCP_CONNECTIONS) {
        return Result::OK;  // table full; no room to accept
    }

    uint32_t slot = m_client_count;
    int ret = mbedtls_net_accept(&m_listen_net, &m_client_net[slot],
                                 nullptr, 0U, nullptr);
    if (ret != 0) {
        // MBEDTLS_ERR_SSL_WANT_READ means no pending connection — not an error
        return Result::OK;
    }

    if (m_tls_enabled) {
        // Set client socket to blocking mode before TLS handshake.
        // mbedtls_net_accept() on a non-blocking listen socket returns a
        // client socket that inherits O_NONBLOCK; the TLS handshake requires
        // blocking I/O to complete its multi-round read/write sequence.
        ret = mbedtls_net_set_block(&m_client_net[slot]);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "net_set_block (client)", ret);
            mbedtls_net_free(&m_client_net[slot]);
            mbedtls_net_init(&m_client_net[slot]);
            return Result::ERR_IO;
        }

        ret = mbedtls_ssl_setup(&m_ssl[slot], &m_ssl_conf);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_setup (accept)", ret);
            mbedtls_net_free(&m_client_net[slot]);
            mbedtls_net_init(&m_client_net[slot]);
            return Result::ERR_IO;
        }
        mbedtls_ssl_set_bio(&m_ssl[slot], &m_client_net[slot],
                            mbedtls_net_send, mbedtls_net_recv, nullptr);

        // TLS handshake (init-phase — Power of 10 Rule 3 deviation documented)
        ret = mbedtls_ssl_handshake(&m_ssl[slot]);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_handshake (accept)", ret);
            mbedtls_ssl_free(&m_ssl[slot]);
            mbedtls_ssl_init(&m_ssl[slot]);
            mbedtls_net_free(&m_client_net[slot]);
            mbedtls_net_init(&m_client_net[slot]);
            return Result::ERR_IO;
        }
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "TLS handshake complete (server slot %u): cipher=%s",
                    slot, mbedtls_ssl_get_ciphersuite(&m_ssl[slot]));
    }

    ++m_client_count;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Accepted client %u (TLS=%s), total=%u",
                slot, m_tls_enabled ? "ON" : "OFF", m_client_count);

    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_client() — close and compact the client table
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::remove_client(uint32_t idx)
{
    NEVER_COMPILED_OUT_ASSERT(idx < m_client_count);
    NEVER_COMPILED_OUT_ASSERT(m_client_count > 0U);

    if (m_tls_enabled) {
        (void)mbedtls_ssl_close_notify(&m_ssl[idx]);
        mbedtls_ssl_free(&m_ssl[idx]);
        mbedtls_ssl_init(&m_ssl[idx]);
    }
    mbedtls_net_free(&m_client_net[idx]);
    mbedtls_net_init(&m_client_net[idx]);

    // Compact: shift remaining slots left
    for (uint32_t j = idx; j < m_client_count - 1U; ++j) {
        m_client_net[j] = m_client_net[j + 1U];
        m_ssl[j]        = m_ssl[j + 1U];
        // Re-initialise the slot we just moved away from
        mbedtls_net_init(&m_client_net[j + 1U]);
        mbedtls_ssl_init(&m_ssl[j + 1U]);
    }
    --m_client_count;
    NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS);
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_send_frame() — 4-byte length prefix + payload; TLS or raw TCP
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::tls_send_frame(uint32_t idx,
                                    const uint8_t* buf, uint32_t len,
                                    uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    // Build 4-byte big-endian length prefix
    uint8_t hdr[4U];
    hdr[0U] = static_cast<uint8_t>((len >> 24U) & 0xFFU);
    hdr[1U] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
    hdr[2U] = static_cast<uint8_t>((len >>  8U) & 0xFFU);
    hdr[3U] = static_cast<uint8_t>( len         & 0xFFU);

    if (m_tls_enabled) {
        // TLS path: write header then payload
        int ret = mbedtls_ssl_write(&m_ssl[idx], hdr, 4U);
        if (ret < 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_write (header)", ret);
            return false;
        }
        ret = mbedtls_ssl_write(&m_ssl[idx], buf,
                                static_cast<size_t>(len));
        if (ret < 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_write (payload)", ret);
            return false;
        }
        return true;
    }

    // Plaintext path: reuse existing SocketUtils framing
    return tcp_send_frame(m_client_net[idx].fd, buf, len, timeout_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_recv_frame() — receive 4-byte length prefix + payload
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::tls_recv_frame(uint32_t idx,
                                    uint8_t* buf, uint32_t buf_cap,
                                    uint32_t timeout_ms, uint32_t* out_len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);
    *out_len = 0U;

    if (!m_tls_enabled) {
        // Plaintext path: reuse existing SocketUtils framing
        return tcp_recv_frame(m_client_net[idx].fd, buf, buf_cap,
                              timeout_ms, out_len);
    }

    // TLS path: read 4-byte header first
    uint8_t hdr[4U];
    int ret = mbedtls_ssl_read(&m_ssl[idx], hdr, 4U);
    if (ret <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            log_mbedtls_err("TlsTcpBackend", "ssl_read (header)", ret);
        }
        return false;
    }

    uint32_t payload_len = (static_cast<uint32_t>(hdr[0U]) << 24U)
                         | (static_cast<uint32_t>(hdr[1U]) << 16U)
                         | (static_cast<uint32_t>(hdr[2U]) <<  8U)
                         |  static_cast<uint32_t>(hdr[3U]);

    if (payload_len == 0U || payload_len > buf_cap) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "tls_recv_frame: invalid length %u (cap=%u)",
                    payload_len, buf_cap);
        return false;
    }

    // Read payload in one ssl_read call (TLS records are typically ≤ 16 KB)
    uint32_t received = 0U;
    // Power of 10 Rule 2: bounded loop — at most payload_len iterations total
    for (uint32_t iter = 0U; iter < payload_len && received < payload_len; ++iter) {
        ret = mbedtls_ssl_read(&m_ssl[idx],
                               buf + received,
                               static_cast<size_t>(payload_len - received));
        if (ret > 0) {
            received += static_cast<uint32_t>(ret);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            continue;
        } else {
            log_mbedtls_err("TlsTcpBackend", "ssl_read (payload)", ret);
            return false;
        }
    }

    if (received != payload_len) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "tls_recv_frame: short read %u/%u", received, payload_len);
        return false;
    }

    *out_len = payload_len;
    NEVER_COMPILED_OUT_ASSERT(*out_len <= buf_cap);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_from_client()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::recv_from_client(uint32_t idx, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(idx < m_client_count);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    uint32_t out_len = 0U;
    if (!tls_recv_frame(idx, m_wire_buf, SOCKET_RECV_BUF_BYTES,
                        timeout_ms, &out_len)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "recv_frame failed on client %u; closing", idx);
        remove_client(idx);
        return Result::ERR_IO;
    }

    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "Deserialize failed on client %u: %u",
                    idx, static_cast<uint8_t>(res));
        return res;
    }

    res = m_recv_queue.push(env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "Recv queue full; dropping message");
    }

    NEVER_COMPILED_OUT_ASSERT(out_len <= SOCKET_RECV_BUF_BYTES);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_to_all_clients()
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::send_to_all_clients(const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    uint32_t timeout_ms = (m_cfg.num_channels > 0U)
                          ? m_cfg.channels[0U].send_timeout_ms
                          : 1000U;

    // Power of 10 Rule 2: fixed loop bound
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        if (!tls_send_frame(i, buf, len, timeout_ms)) {
            Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                        "Send frame failed on client %u", i);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_clients() / flush_delayed_to_queue()
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::flush_delayed_to_clients(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        uint32_t delayed_len = 0U;
        Result res = Serializer::serialize(delayed[i], m_wire_buf,
                                           SOCKET_RECV_BUF_BYTES, delayed_len);
        if (!result_ok(res)) {
            continue;
        }
        send_to_all_clients(m_wire_buf, delayed_len);
    }
}

void TlsTcpBackend::flush_delayed_to_queue(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        (void)m_recv_queue.push(delayed[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// poll_clients_once()
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::poll_clients_once(uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(timeout_ms <= 60000U);

    if (m_is_server) {
        // When no clients are connected, wait on the listen socket so the
        // receive_message() polling loop does not spin in zero time before the
        // client has had a chance to connect.  mbedtls_net_poll returns as soon
        // as a connection is pending or the timeout expires.
        if (m_client_count == 0U) {
            (void)mbedtls_net_poll(&m_listen_net, MBEDTLS_NET_POLL_READ, timeout_ms);
        }
        (void)accept_and_handshake();
    }

    // Power of 10 Rule 2: fixed loop bound
    // Iterate backwards to avoid index shift from remove_client() inside the loop
    uint32_t i = m_client_count;
    while (i > 0U) {
        --i;
        (void)recv_from_client(i, timeout_ms);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP);
    NEVER_COMPILED_OUT_ASSERT(!m_open);

    m_cfg          = config;
    m_is_server    = config.is_server;
    m_tls_enabled  = config.tls.tls_enabled;

    m_recv_queue.init();

    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg.enabled = config.channels[0U].impairments_enabled;
    }
    m_impairment.init(imp_cfg);

    if (m_tls_enabled) {
        Result res = setup_tls_config(config.tls);
        if (!result_ok(res)) {
            return res;
        }
    }

    Result res = m_is_server
                 ? bind_and_listen(config.bind_ip, config.bind_port)
                 : connect_to_server();

    if (!result_ok(res)) {
        return res;
    }

    NEVER_COMPILED_OUT_ASSERT(m_open);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));

    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(envelope, m_wire_buf,
                                       SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend", "Serialize failed");
        return res;
    }

    uint64_t now_us = timestamp_now_us();
    res = m_impairment.process_outbound(envelope, now_us);
    if (res == Result::ERR_IO) {
        return Result::OK;  // dropped by impairment engine
    }

    if (m_client_count == 0U) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "No clients; discarding message");
        return Result::OK;
    }

    send_to_all_clients(m_wire_buf, wire_len);
    flush_delayed_to_clients(now_us);

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);

    Result res = m_recv_queue.pop(envelope);
    if (result_ok(res)) {
        return res;
    }

    uint32_t poll_count = (timeout_ms + 99U) / 100U;
    if (poll_count > 50U) { poll_count = 50U; }
    NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U);

    for (uint32_t attempt = 0U; attempt < poll_count; ++attempt) {
        poll_clients_once(100U);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) { return res; }

        uint64_t now_us = timestamp_now_us();
        flush_delayed_to_queue(now_us);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) { return res; }
    }

    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// close()
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::close()
{
    if (m_tls_enabled) {
        for (uint32_t i = 0U; i < m_client_count; ++i) {
            (void)mbedtls_ssl_close_notify(&m_ssl[i]);
            mbedtls_ssl_free(&m_ssl[i]);
            mbedtls_ssl_init(&m_ssl[i]);
        }
    }
    // Free all client net contexts
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_free(&m_client_net[i]);
        mbedtls_net_init(&m_client_net[i]);
    }
    mbedtls_net_free(&m_listen_net);
    mbedtls_net_init(&m_listen_net);
    m_client_count = 0U;
    m_open         = false;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Transport closed (TLS=%s)", m_tls_enabled ? "ON" : "OFF");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(
        m_open == (m_listen_net.fd >= 0 || m_client_count > 0U) || !m_open);
    return m_open;
}
