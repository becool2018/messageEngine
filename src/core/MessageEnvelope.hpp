/**
 * @file MessageEnvelope.hpp
 * @brief Standard message envelope carried by every application message.
 *
 * Requirement traceability: CLAUDE.md §3.1 (Message envelope).
 *
 * Rules applied:
 *   - Power of 10 rules 3, 6, 9: fixed-size payload, minimal scope, ≤1 ptr level.
 *   - MISRA C++:2008 rule 0-1-1: every field is used.
 *   - F-Prime style: no templates, no STL, no exceptions.
 *
 * Implements: REQ-3.1
 */

#ifndef CORE_MESSAGE_ENVELOPE_HPP
#define CORE_MESSAGE_ENVELOPE_HPP

#include <cstdint>
#include <cstring>
#include "Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// MessageEnvelope
// ─────────────────────────────────────────────────────────────────────────────
// Fields ordered by decreasing alignment to minimise padding (1 byte optimal).
// Serializer reads/writes each field explicitly — wire format is independent
// of this in-memory layout.
struct MessageEnvelope {
    uint64_t          message_id;                        ///< unique per (source, session)
    uint64_t          timestamp_us;                      ///< creation time (µs since epoch)
    uint64_t          expiry_time_us;                    ///< drop after this wall-clock µs
    NodeId            source_id;                         ///< originating node
    NodeId            destination_id;                    ///< target node (0 = broadcast)
    uint32_t          payload_length;                    ///< bytes used in payload[]
    MessageType       message_type;                      ///< DATA / ACK / NAK / HEARTBEAT
    uint8_t           priority;                          ///< 0 = highest
    ReliabilityClass  reliability_class;
    uint8_t           payload[MSG_MAX_PAYLOAD_BYTES];    ///< opaque serialized bytes
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers (no heap, no function pointers – Power of 10 rules 3, 9)
// ─────────────────────────────────────────────────────────────────────────────

// Safety-critical (SC): HAZ-005
/// Zero-initialise an envelope.  Called during init phase.
inline void envelope_init(MessageEnvelope& env)
{
    // Power of 10: use memset; bounded by sizeof
    (void)memset(&env, 0, sizeof(MessageEnvelope));
    env.message_type = MessageType::INVALID;
}

// Safety-critical (SC): HAZ-002, HAZ-003
/// Shallow copy src → dst.
inline void envelope_copy(MessageEnvelope& dst, const MessageEnvelope& src)
{
    (void)memcpy(&dst, &src, sizeof(MessageEnvelope));
}

// Safety-critical (SC): HAZ-001, HAZ-005
/// Return true when the envelope has a valid message type.
inline bool envelope_valid(const MessageEnvelope& env)
{
    return (env.message_type != MessageType::INVALID) &&
           (env.payload_length <= MSG_MAX_PAYLOAD_BYTES) &&
           (env.source_id      != NODE_ID_INVALID);
}

// Safety-critical (SC): HAZ-001, HAZ-004
/// Return true when the envelope carries application data (not control).
inline bool envelope_is_data(const MessageEnvelope& env)
{
    return env.message_type == MessageType::DATA;
}

// Safety-critical (SC): HAZ-001
/// Return true for in-band control messages (ACK / NAK / HEARTBEAT).
inline bool envelope_is_control(const MessageEnvelope& env)
{
    return (env.message_type == MessageType::ACK)      ||
           (env.message_type == MessageType::NAK)      ||
           (env.message_type == MessageType::HEARTBEAT);
}

// Safety-critical (SC): HAZ-002
/// Build a minimal ACK envelope targeting the source of @p original.
inline void envelope_make_ack(MessageEnvelope&       ack,
                               const MessageEnvelope& original,
                               NodeId                 my_id,
                               uint64_t               now_us)
{
    envelope_init(ack);
    ack.message_type       = MessageType::ACK;
    ack.message_id         = original.message_id; // ACKing this ID
    ack.timestamp_us       = now_us;
    ack.source_id          = my_id;
    ack.destination_id     = original.source_id;
    ack.priority           = original.priority;
    ack.reliability_class  = ReliabilityClass::BEST_EFFORT;
    ack.expiry_time_us     = 0U;  // ACKs do not expire
    ack.payload_length     = 0U;
}

#endif // CORE_MESSAGE_ENVELOPE_HPP
