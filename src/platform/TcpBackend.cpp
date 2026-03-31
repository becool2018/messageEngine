/**
 * @file TcpBackend.cpp
 * @brief Implementation of TCP transport backend.
 *
 * Implements connection management, framing, and impairment simulation
 * for message delivery over TCP.
 *
 * Rules applied:
 *   - Power of 10: all functions ≤1 page, ≥2 assertions each, fixed loop bounds.
 *   - MISRA C++: no exceptions, all return values checked.
 *   - F-Prime style: event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.4, REQ-6.1.5, REQ-6.1.6, REQ-6.1.7, REQ-6.3.5, REQ-7.1.1, REQ-7.2.4
 */

#include "platform/TcpBackend.hpp"
#include "platform/ISocketOps.hpp"
#include "platform/SocketOpsImpl.hpp"
#include "platform/SocketUtils.hpp"
#include "core/Assert.hpp"
#include "core/Serializer.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include <poll.h>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

TcpBackend::TcpBackend()
    : m_sock_ops(&SocketOpsImpl::instance()),
      m_listen_fd(-1), m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);  // Invariant
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_fds[i] = -1;
    }
}

TcpBackend::TcpBackend(ISocketOps& ops)
    : m_sock_ops(&ops),
      m_listen_fd(-1), m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);  // Invariant
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_fds[i] = -1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

TcpBackend::~TcpBackend()
{
    // Power of 10: explicit qualified call avoids virtual dispatch in destructor
    // (static dispatch to concrete implementation is the intended behaviour here)
    TcpBackend::close();
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::bind_and_listen(const char* ip, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);          // Power of 10: valid pointer
    NEVER_COMPILED_OUT_ASSERT(m_is_server);            // Server mode only

    m_listen_fd = m_sock_ops->create_tcp();
    if (m_listen_fd < 0) {
        Logger::log(Severity::FATAL, "TcpBackend", "socket_create_tcp failed in server mode");
        return Result::ERR_IO;
    }
    if (!m_sock_ops->set_reuseaddr(m_listen_fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend", "socket_set_reuseaddr failed");
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    if (!m_sock_ops->do_bind(m_listen_fd, ip, port)) {
        Logger::log(Severity::FATAL, "TcpBackend", "socket_bind failed on port %u", port);
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    if (!m_sock_ops->do_listen(m_listen_fd, static_cast<int>(MAX_TCP_CONNECTIONS))) {
        Logger::log(Severity::FATAL, "TcpBackend", "socket_listen failed");
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    // Make listen fd non-blocking so accept_clients() returns immediately
    // when no pending connection exists, allowing receive_message() to honour
    // its timeout. Power of 10 Rule 2 deviation: infrastructure poll loop.
    if (!m_sock_ops->set_nonblocking(m_listen_fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "socket_set_nonblocking failed on listen fd");
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    m_open = true;
    Logger::log(Severity::INFO, "TcpBackend", "Server listening on %s:%u", ip, port);
    NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0);       // Power of 10: post-condition
    return Result::OK;
}

Result TcpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(!m_open);  // Not already initialized

    m_cfg       = config;
    m_is_server = config.is_server;

    // Initialize receive queue and impairment engine
    m_recv_queue.init();

    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg = config.channels[0U].impairment;
    }
    m_impairment.init(imp_cfg);

    Result res;
    if (m_is_server) {
        res = bind_and_listen(config.bind_ip, config.bind_port);
    } else {
        res = connect_to_server();
    }
    if (!result_ok(res)) {
        return res;
    }

    NEVER_COMPILED_OUT_ASSERT(m_open);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// connect_to_server()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::connect_to_server()
{
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);  // Client mode only
    NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] == -1);  // Not yet connected

    int fd = m_sock_ops->create_tcp();
    if (fd < 0) {
        Logger::log(Severity::FATAL, "TcpBackend",
                   "socket_create_tcp failed in client mode");
        return Result::ERR_IO;
    }

    if (!m_sock_ops->set_reuseaddr(fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                   "socket_set_reuseaddr failed");
        m_sock_ops->do_close(fd);
        return Result::ERR_IO;
    }

    if (!m_sock_ops->connect_with_timeout(fd, m_cfg.peer_ip, m_cfg.peer_port,
                                          m_cfg.connect_timeout_ms)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                   "Connection to %s:%u failed", m_cfg.peer_ip, m_cfg.peer_port);
        m_sock_ops->do_close(fd);
        return Result::ERR_IO;
    }

    m_client_fds[0U] = fd;
    m_client_count   = 1U;
    m_open           = true;

    Logger::log(Severity::INFO, "TcpBackend",
               "Connected to %s:%u", m_cfg.peer_ip, m_cfg.peer_port);

    NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] >= 0);  // Post-condition
    NEVER_COMPILED_OUT_ASSERT(m_client_count == 1U);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// accept_clients()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::accept_clients()
{
    NEVER_COMPILED_OUT_ASSERT(m_is_server);  // Server mode only
    NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0);  // Listening socket must be open

    if (m_client_count >= MAX_TCP_CONNECTIONS) {
        // Already at capacity; don't try to accept
        return Result::OK;
    }

    int client_fd = m_sock_ops->do_accept(m_listen_fd);
    if (client_fd < 0) {
        // No pending connection (EAGAIN in non-blocking mode) is not an error
        return Result::OK;
    }

    // Add new client to array
    NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS);
    m_client_fds[m_client_count] = client_fd;
    ++m_client_count;

    Logger::log(Severity::INFO, "TcpBackend",
               "Accepted client %u, total clients: %u", m_client_count - 1U, m_client_count);

    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_client_fd() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::remove_client_fd(int client_fd)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);           // Power of 10: valid descriptor
    NEVER_COMPILED_OUT_ASSERT(m_client_count > 0U);      // Power of 10: at least one client exists

    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] == client_fd) {
            m_sock_ops->do_close(m_client_fds[i]);
            m_client_fds[i] = -1;
            // Compact the fd array by shifting left
            for (uint32_t j = i; j < m_client_count - 1U; ++j) {
                m_client_fds[j] = m_client_fds[j + 1U];
            }
            m_client_fds[m_client_count - 1U] = -1;
            --m_client_count;
            NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS);  // Power of 10: post-condition
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_from_client()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::recv_from_client(int client_fd, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);  // Power of 10: valid fd
    NEVER_COMPILED_OUT_ASSERT(m_open);          // Power of 10: transport must be open

    uint32_t out_len = 0U;
    if (!m_sock_ops->recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, timeout_ms, &out_len)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend", "recv_frame failed; closing connection");
        remove_client_fd(client_fd);
        return Result::ERR_IO;
    }

    // Deserialize and queue the received frame
    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
                   "Deserialize failed: %u", static_cast<uint8_t>(res));
        return res;
    }

    res = m_recv_queue.push(env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend", "Recv queue full; dropping message");
    }

    NEVER_COMPILED_OUT_ASSERT(out_len <= SOCKET_RECV_BUF_BYTES);  // Power of 10: post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_to_all_clients() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::send_to_all_clients(const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);         // Power of 10: valid buffer
    NEVER_COMPILED_OUT_ASSERT(len > 0U);               // Power of 10: non-empty frame

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] < 0) {
            continue;
        }
        if (!m_sock_ops->send_frame(m_client_fds[i], buf, len,
                                    m_cfg.channels[0U].send_timeout_ms)) {
            Logger::log(Severity::WARNING_LO, "TcpBackend",
                       "Send frame failed on client %u", i);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_clients() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::flush_delayed_to_clients(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);          // Power of 10: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(m_open);                 // Power of 10: transport must be open

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);

    // Power of 10 rule 2: fixed loop bound
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

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_queue() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::flush_delayed_to_queue(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);          // Power of 10: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(m_open);                 // Power of 10: transport must be open

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        (void)m_recv_queue.push(delayed[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// poll_clients_once() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::poll_clients_once(uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);                  // Power of 10: transport must be open
    NEVER_COMPILED_OUT_ASSERT(timeout_ms <= 60000U);    // Power of 10: reasonable per-poll timeout

    // Build a pollfd set: listen fd (server, not at capacity) + all client fds.
    // This replaces the prior blocking accept() design. The listen fd is non-blocking
    // (set in init()); poll() provides the per-iteration wait.
    // Power of 10: fixed-size stack array; bounded build loop.
    static const uint32_t MAX_POLL_FDS = MAX_TCP_CONNECTIONS + 1U;
    struct pollfd pfds[MAX_POLL_FDS];
    uint32_t nfds = 0U;

    bool has_listen = m_is_server && (m_listen_fd >= 0) &&
                      (m_client_count < MAX_TCP_CONNECTIONS);
    if (has_listen) {
        pfds[0U].fd      = m_listen_fd;
        pfds[0U].events  = POLLIN;
        pfds[0U].revents = 0;
        nfds = 1U;
    }
    // Power of 10 Rule 2: fixed bound (MAX_TCP_CONNECTIONS)
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] >= 0) {
            NEVER_COMPILED_OUT_ASSERT(nfds < MAX_POLL_FDS);
            pfds[nfds].fd      = m_client_fds[i];
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
    }
    NEVER_COMPILED_OUT_ASSERT(nfds <= MAX_POLL_FDS);

    if (nfds == 0U) {
        return;  // No fds to watch; outer loop handles retry timing
    }

    // Block until a fd is readable or timeout_ms elapses.
    // Power of 10 Rule 2 deviation: infrastructure poll; bounded per-call timeout.
    int poll_rc = poll(pfds, static_cast<nfds_t>(nfds),
                       static_cast<int>(timeout_ms));
    if (poll_rc <= 0) {
        return;  // Timeout (0) or error (−1): nothing to accept or receive
    }

    // Accept new connection if listen fd is readable
    if (has_listen && ((pfds[0U].revents & POLLIN) != 0)) {
        (void)accept_clients();
    }

    // Receive from any connected client fd that is now readable.
    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] >= 0) {
            (void)recv_from_client(m_client_fds[i], timeout_ms);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// send_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);                    // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));  // Pre-condition

    // Serialize to wire buffer
    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(envelope, m_wire_buf,
                                       SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend", "Serialize failed");
        return res;
    }

    // Apply impairment: process_outbound may drop the message (ERR_IO = drop silently)
    uint64_t now_us = timestamp_now_us();
    res = m_impairment.process_outbound(envelope, now_us);
    if (res == Result::ERR_IO) {
        return Result::OK;  // Message dropped by loss impairment
    }

    // Discard if no clients are connected
    if (m_client_count == 0U) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
                   "No clients connected; discarding message");
        return Result::OK;
    }

    // process_outbound() already queued the message into the impairment delay
    // buffer (with release_us = now_us when latency is 0). flush_delayed_to_clients()
    // collects all deliverable messages and sends them — covering both the 0-delay
    // pass-through case and the delayed case.  Calling send_to_all_clients() here as
    // well would cause every message to be sent twice.
    flush_delayed_to_clients(now_us);

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Pre-condition

    // Check receive queue first
    Result res = m_recv_queue.pop(envelope);
    if (result_ok(res)) {
        return res;
    }

    uint32_t poll_count = (timeout_ms + 99U) / 100U;
    if (poll_count > 50U) {
        poll_count = 50U;  // Cap at 5 seconds worth
    }
    NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U);  // Power of 10: bounded loop count

    // Power of 10 rule 2: fixed loop bound (capped above)
    for (uint32_t attempt = 0U; attempt < poll_count; ++attempt) {
        poll_clients_once(100U);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) {
            return res;
        }

        uint64_t now_us = timestamp_now_us();
        flush_delayed_to_queue(now_us);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) {
            return res;
        }
    }

    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// close()
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::close()
{
    if (m_listen_fd >= 0) {
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
    }

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] >= 0) {
            m_sock_ops->do_close(m_client_fds[i]);
            m_client_fds[i] = -1;
        }
    }

    m_client_count = 0U;
    m_open         = false;
    Logger::log(Severity::INFO, "TcpBackend", "Transport closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool TcpBackend::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(m_open == (m_listen_fd >= 0 || m_client_count > 0U) || !m_open);
    return m_open;
}
