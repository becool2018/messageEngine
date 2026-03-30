/**
 * @file AckTracker.hpp
 * @brief Tracks outstanding messages awaiting ACK, detects timeouts and retries.
 *
 * Requirement traceability: CLAUDE.md §3.2 (ACK handling, retry logic),
 * messageEngine/CLAUDE.md §3.3 (reliable delivery semantics).
 *
 * Rules applied:
 *   - Power of 10: fixed capacity (ACK_TRACKER_CAPACITY), no dynamic allocation,
 *     ≥2 assertions per method, provable loop bounds.
 *   - MISRA C++: no STL, no exceptions, ≤1 ptr indirection.
 *   - F-Prime style: simple state machine (FREE/PENDING/ACKED), deterministic behavior.
 *
 * Implements: REQ-3.2.4, REQ-3.3.2
 */

#ifndef CORE_ACK_TRACKER_HPP
#define CORE_ACK_TRACKER_HPP

#include <cstdint>
#include <cstring>
#include "Assert.hpp"
#include "Types.hpp"
#include "MessageEnvelope.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// AckTracker: tracks outstanding messages awaiting ACK, timeout management
// ─────────────────────────────────────────────────────────────────────────────

class AckTracker {
public:
    /// Initialize the tracker (zero all state).
    /// Must be called once during system initialization.
    void init();

    // Safety-critical (SC): HAZ-002, HAZ-006
    /// Add a message to be tracked for ACK receipt.
    /// @param env         [in] message being tracked
    /// @param deadline_us [in] absolute time after which message is considered timed out
    /// @return OK on success; ERR_FULL if ACK_TRACKER_CAPACITY is reached
    Result track(const MessageEnvelope& env, uint64_t deadline_us);

    // Safety-critical (SC): HAZ-002
    /// Mark a message as ACKed.
    /// @param src    [in] source (sender) of the message
    /// @param msg_id [in] message ID that was ACKed
    /// @return OK on success; ERR_INVALID if no matching pending message found
    Result on_ack(NodeId src, uint64_t msg_id);

    // Safety-critical (SC): HAZ-002, HAZ-006
    /// Remove all expired entries and return them for retry/failure handling.
    /// @param now_us      [in] current time
    /// @param expired_buf [out] buffer to store expired envelopes
    /// @param buf_cap     [in] capacity of expired_buf
    /// @return number of expired entries removed (0 to buf_cap)
    uint32_t sweep_expired(uint64_t         now_us,
                          MessageEnvelope* expired_buf,
                          uint32_t         buf_cap);

private:
    /// Process one slot during sweep_expired.
    /// Copies expired PENDING envelope to buf (if space), releases ACKED slots.
    /// @return 1 if an expired envelope was added to buf, 0 otherwise.
    uint32_t sweep_one_slot(uint32_t         idx,
                            uint64_t         now_us,
                            MessageEnvelope* expired_buf,
                            uint32_t         buf_cap,
                            uint32_t         expired_count);

    // Power of 10 rule 9: ≤1 pointer indirection; using simple fixed array
    /// Entry state machine
    enum class EntryState : uint8_t {
        FREE = 0U,     ///< slot unused
        PENDING = 1U,  ///< message sent, awaiting ACK
        ACKED = 2U     ///< message ACKed; slot will be freed on next sweep
    };

    /// Entry in the tracker
    struct Entry {
        MessageEnvelope env;         // original message being tracked
        uint64_t        deadline_us; // absolute time of ACK timeout
        EntryState      state;       // FREE, PENDING, or ACKED
    };

    // Power of 10 rule 3: fixed-size allocation at static scope
    Entry    m_slots[ACK_TRACKER_CAPACITY];  ///< fixed array of tracker entries
    uint32_t m_count;                        ///< number of non-FREE slots currently in use
};

#endif // CORE_ACK_TRACKER_HPP
