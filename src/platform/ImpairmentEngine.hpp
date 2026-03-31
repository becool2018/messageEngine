/**
 * @file ImpairmentEngine.hpp
 * @brief Impairment simulation engine for injecting network faults.
 *
 * Sits logically between transport abstraction and underlying I/O.
 * Applies configurable impairments (latency, jitter, loss, duplication, reordering, partitions)
 * to messages in a deterministic manner controlled by a seedable PRNG.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §5 (Impairment engine requirements).
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation after init, fixed buffer sizes, ≥2 assertions per method.
 *   - MISRA C++: no STL, no exceptions, no templates, ≤1 pointer indirection.
 *   - F-Prime style: Result enum return codes, event/logging model.
 *
 * Implements: REQ-5.1.1, REQ-5.1.2, REQ-5.1.3, REQ-5.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.2.2, REQ-5.3.1, REQ-5.3.2
 */

#ifndef PLATFORM_IMPAIRMENT_ENGINE_HPP
#define PLATFORM_IMPAIRMENT_ENGINE_HPP

#include <cstdint>
#include "core/Assert.hpp"
#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "platform/PrngEngine.hpp"
#include "core/ImpairmentConfig.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ImpairmentEngine: applies network impairments to messages
// ─────────────────────────────────────────────────────────────────────────────

class ImpairmentEngine {
public:
    /// Default constructor — sets all state to safe uninitialised values.
    /// Calling any method other than init() before init() is a contract violation.
    ImpairmentEngine();

    /**
     * @brief Initialize the impairment engine.
     *
     * Seeds the PRNG, zeroes delay and reorder buffers, resets partition state.
     * Called once during system initialization.
     *
     * Power of 10: allocation phase only; all state is pre-allocated.
     *
     * @param[in] cfg Configuration struct specifying enabled impairments and parameters.
     */
    void init(const ImpairmentConfig& cfg);

    // Safety-critical (SC): HAZ-002, HAZ-007
    /**
     * @brief Process an outbound message through the impairment engine.
     *
     * Applies loss, duplication, and latency/jitter impairments to a message
     * being sent. Messages destined for delayed delivery are buffered internally;
     * latency is handled by the caller polling collect_deliverable().
     *
     * Power of 10: fixed loop bounds, checked returns, ≥2 assertions.
     *
     * @param[in] in_env   Message envelope to process.
     * @param[in] now_us   Current time in microseconds.
     * @return OK if message accepted (may be in delay buffer); ERR_IO if dropped
     *         due to loss or partition; ERR_FULL if delay buffer full.
     */
    Result process_outbound(const MessageEnvelope& in_env, uint64_t now_us);

    // Safety-critical (SC): HAZ-004, HAZ-006
    /**
     * @brief Collect messages ready for delivery from the delay buffer.
     *
     * Scans the internal delay buffer for entries whose release_us <= now_us,
     * copies them to out_buf, and removes them from the delay buffer.
     *
     * Power of 10: fixed loop bounds, ≥2 assertions.
     *
     * @param[in] now_us     Current time in microseconds.
     * @param[out] out_buf   Buffer to receive deliverable messages.
     * @param[in] buf_cap    Number of MessageEnvelope slots in out_buf.
     * @return Number of messages copied to out_buf (0 to buf_cap).
     */
    uint32_t collect_deliverable(uint64_t now_us,
                                  MessageEnvelope* out_buf,
                                  uint32_t buf_cap);

    // Safety-critical (SC): HAZ-003, HAZ-007
    /**
     * @brief Process an inbound message through reordering simulation.
     *
     * If reordering is enabled, buffers the message in an internal reorder window
     * and may randomly release a different buffered message instead of in_env.
     * If reordering is disabled, passes through immediately.
     *
     * Power of 10: fixed loop bounds, ≥2 assertions.
     *
     * @param[in] in_env      Message envelope received.
     * @param[in] now_us      Current time in microseconds (unused in reordering logic).
     * @param[out] out_buf    Buffer to receive reordered message(s).
     * @param[in] buf_cap     Number of MessageEnvelope slots in out_buf.
     * @param[out] out_count  Number of envelopes written to out_buf.
     * @return OK on success; ERR_FULL if reorder buffer full (should not happen with correct sizing).
     */
    Result process_inbound(const MessageEnvelope& in_env,
                          uint64_t now_us,
                          MessageEnvelope* out_buf,
                          uint32_t buf_cap,
                          uint32_t& out_count);

    // Safety-critical (SC): HAZ-007
    /**
     * @brief Check if a partition (outage) is currently active.
     *
     * Maintains partition state machine: transitions between active/inactive
     * based on wall-clock time and configured duration/gap. Initializes the
     * first partition event on first call.
     *
     * Power of 10: no loops, ≥2 assertions, state machine logic.
     *
     * @param[in] now_us Current time in microseconds.
     * @return true if partition is active (traffic should be dropped).
     */
    bool is_partition_active(uint64_t now_us);

    /**
     * @brief Accessor for the current configuration.
     *
     * @return const reference to m_cfg.
     */
    // cppcheck-suppress unusedFunction -- called from tests/test_ImpairmentEngine.cpp
    const ImpairmentConfig& config() const { return m_cfg; }

private:
    // ───────────────────────────────────────────────────────────────────────
    // Private helper methods (Power of 10: small, single-purpose functions)
    // ───────────────────────────────────────────────────────────────────────

    /// Find the first inactive slot in m_delay_buf and queue env for delivery
    /// at release_us. Caller must ensure m_delay_count < IMPAIR_DELAY_BUF_SIZE.
    /// @return OK on success; ERR_FULL on logic error (no free slot found).
    Result queue_to_delay_buf(const MessageEnvelope& env, uint64_t release_us);

    /// Check whether the current message should be dropped by loss impairment.
    /// Includes the probability guard; returns false if loss_probability is zero.
    /// @return true if the message should be dropped.
    bool check_loss();

    /// Probabilistically queue a duplicate copy of env into the delay buffer.
    /// No-op if duplication roll fails or delay buffer is full.
    /// @param[in] env        Original message envelope to duplicate.
    /// @param[in] release_us Release timestamp of the original (duplicate offset by 100 µs).
    void apply_duplication(const MessageEnvelope& env, uint64_t release_us);
    // ───────────────────────────────────────────────────────────────────────
    // Delay buffer entry (messages waiting for release due to latency/jitter)
    // ───────────────────────────────────────────────────────────────────────
    struct DelayEntry {
        MessageEnvelope env;           ///< buffered message
        uint64_t        release_us;    ///< wall-clock µs at which to release
        bool            active;        ///< true = valid entry; false = empty slot
    };

    // ───────────────────────────────────────────────────────────────────────
    // Internal state
    // ───────────────────────────────────────────────────────────────────────
    ImpairmentConfig m_cfg;
    PrngEngine       m_prng;
    DelayEntry       m_delay_buf[IMPAIR_DELAY_BUF_SIZE];   ///< latency/jitter buffer
    uint32_t         m_delay_count;                        ///< count of active entries
    MessageEnvelope  m_reorder_buf[IMPAIR_DELAY_BUF_SIZE]; ///< reordering window
    uint32_t         m_reorder_count;                      ///< count of buffered messages
    bool             m_partition_active;                   ///< true if in partition state
    uint64_t         m_partition_start_us;                 ///< wall-clock µs partition started
    uint64_t         m_next_partition_event_us;            ///< next state-change time
    bool             m_initialized;                        ///< true after init()
};

#endif // PLATFORM_IMPAIRMENT_ENGINE_HPP
