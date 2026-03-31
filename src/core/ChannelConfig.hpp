/**
 * @file ChannelConfig.hpp
 * @brief Per-channel configuration: priority, reliability, ordering, limits.
 *
 * Requirement traceability: CLAUDE.md §4.2 (Configuration and channels).
 *
 * Rules applied:
 *   - Power of 10 rule 3: all limits are compile-time constants.
 *   - F-Prime style: plain struct, no STL.
 *
 * Implements: REQ-3.3.5, REQ-4.2.1, REQ-4.2.2, REQ-6.3.4
 */

#ifndef CORE_CHANNEL_CONFIG_HPP
#define CORE_CHANNEL_CONFIG_HPP

#include <cstdint>
#include "Types.hpp"
#include "TlsConfig.hpp"
#include "ImpairmentConfig.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ChannelConfig
// ─────────────────────────────────────────────────────────────────────────────
struct ChannelConfig {
    uint8_t          channel_id;           ///< 0-based index, < MAX_CHANNELS
    uint8_t          priority;             ///< 0 = highest priority
    ReliabilityClass reliability;
    OrderingMode     ordering;
    uint32_t         max_queue_depth;      ///< must be ≤ MSG_RING_CAPACITY
    uint32_t         send_timeout_ms;
    uint32_t         recv_timeout_ms;
    uint32_t         max_retries;          ///< ignored when best-effort
    uint32_t         retry_backoff_ms;     ///< initial retry interval; doubled each attempt
    ImpairmentConfig impairment;           ///< full impairment configuration for this channel
};

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig – top-level config passed to TransportInterface::init()
// ─────────────────────────────────────────────────────────────────────────────
struct TransportConfig {
    TransportKind   kind;
    char            bind_ip[48];    ///< IPv4/IPv6 address string
    uint16_t        bind_port;
    char            peer_ip[48];    ///< for client-side TCP/UDP
    uint16_t        peer_port;
    uint32_t        connect_timeout_ms;
    uint32_t        num_channels;           ///< ≤ MAX_CHANNELS
    ChannelConfig   channels[MAX_CHANNELS];
    NodeId          local_node_id;
    bool            is_server;
    TlsConfig       tls;   ///< TLS configuration (REQ-6.3.4); tls_enabled=false → plaintext
};

/// Default-fill a ChannelConfig with safe values.
inline void channel_config_default(ChannelConfig& cfg, uint8_t id)
{
    cfg.channel_id         = id;
    cfg.priority           = static_cast<uint8_t>(id);   // lower id = higher priority
    cfg.reliability        = ReliabilityClass::BEST_EFFORT;
    cfg.ordering           = OrderingMode::UNORDERED;
    cfg.max_queue_depth    = MSG_RING_CAPACITY;
    cfg.send_timeout_ms    = 1000U;
    cfg.recv_timeout_ms    = 1000U;
    cfg.max_retries        = MAX_RETRY_COUNT;
    cfg.retry_backoff_ms   = 100U;
    impairment_config_default(cfg.impairment);
}

/// Default-fill a TransportConfig for local loopback TCP.
inline void transport_config_default(TransportConfig& cfg)
{
    (void)__builtin_memset(&cfg, 0, sizeof(TransportConfig));
    cfg.kind              = TransportKind::TCP;
    cfg.bind_port         = 9000U;
    cfg.peer_port         = 9000U;
    cfg.connect_timeout_ms = 5000U;
    cfg.num_channels      = 1U;
    cfg.local_node_id     = 1U;
    cfg.is_server         = false;

    // initialise bind_ip / peer_ip to loopback
    // Power of 10 Rule 2: bound by the source literal length, not the destination
    // capacity, so the loop cannot read past the end of the string literal.
    // cppcheck arrayIndexOutOfBoundsCond fix: was `i < 47` (destination bound);
    // corrected to `i < LO_MAX` (source bound). sizeof includes the null terminator,
    // so LO_MAX == 9, which is the exact character count of "127.0.0.1".
    const char* lo = "127.0.0.1";
    static const int LO_MAX = static_cast<int>(sizeof("127.0.0.1")) - 1;
    int i = 0;
    while (lo[i] != '\0' && i < LO_MAX) {
        cfg.bind_ip[i] = lo[i];
        cfg.peer_ip[i] = lo[i];
        ++i;
    }
    cfg.bind_ip[i] = '\0';
    cfg.peer_ip[i] = '\0';

    channel_config_default(cfg.channels[0], 0U);
    tls_config_default(cfg.tls);
}

#endif // CORE_CHANNEL_CONFIG_HPP
