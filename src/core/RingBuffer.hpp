/**
 * @file RingBuffer.hpp
 * @brief Fixed-capacity, statically-allocated SPSC ring buffer for MessageEnvelope.
 *
 * Implements a single-producer, single-consumer (SPSC) lock-free FIFO using
 * std::atomic<uint32_t> with acquire/release memory ordering.
 *
 * Why std::atomic rather than GCC __atomic built-ins
 * --------------------------------------------------
 * F-Prime's core principle is to "use constructs that are portable and
 * well-defined across toolchains."  std::atomic<T> satisfies that principle:
 *   - ISO standard since C++11; fully covered by MISRA C++:2023.
 *   - Portable across GCC, Clang, MSVC, IAR, and any other C++17 compiler.
 *   - No dynamic allocation; maps directly to a hardware primitive.
 * GCC __atomic built-ins are compiler-specific and require a MISRA deviation
 * for every use site.  std::atomic requires none.
 *
 * Memory ordering guarantees
 * --------------------------
 * Producer path (push):
 *   1. Loads m_head with acquire to observe freed slots from the consumer.
 *   2. Writes envelope data into m_buf[].
 *   3. Stores new m_tail with release — the consumer's acquire load of m_tail
 *      synchronises-with this store, ensuring all written data is visible
 *      before pop() reads it.
 *
 * Consumer path (pop / peek):
 *   1. Loads m_tail with acquire to observe filled slots from the producer.
 *   2. Reads envelope data from m_buf[].
 *   3. Stores new m_head with release — the producer's acquire load of m_head
 *      synchronises-with this store, ensuring the freed slot is visible
 *      before push() writes into it again.
 *
 * Occupancy is derived from (m_tail - m_head) using unsigned 32-bit arithmetic,
 * which wraps correctly at 2^32.  m_count has been eliminated to remove the
 * shared read-write state that was the source of the original data race.
 *
 * Thread-safety contract
 * ----------------------
 *   - Exactly one thread may call push() / full()            (producer role).
 *   - Exactly one thread may call pop() / peek() / empty()  (consumer role).
 *   - size() is an approximate snapshot; safe for diagnostics from either side.
 *
 * Rules applied:
 *   - Power of 10 rule 2: no unbounded loops; all index arithmetic is O(1).
 *   - Power of 10 rule 3: no dynamic allocation; storage is inline.
 *   - Power of 10 rule 5: >= 2 assertions per non-trivial function.
 *   - F-Prime: std::atomic<uint32_t> is the approved carve-out from the
 *     no-STL rule; see CLAUDE.md section 4.
 *   - MISRA C++:2023: std::atomic is the endorsed approach for lock-free state.
 *
 * Capacity is MSG_RING_CAPACITY (must be a power of two).
 *
 * Implements: REQ-4.1.2
 */

#ifndef CORE_RING_BUFFER_HPP
#define CORE_RING_BUFFER_HPP

#include <atomic>
#include "Assert.hpp"
#include <cstdint>
#include "MessageEnvelope.hpp"
#include "Types.hpp"

// MSG_RING_CAPACITY must be a power of two (asserted at init).
static const uint32_t RING_MASK = MSG_RING_CAPACITY - 1U;

class RingBuffer {
public:
    RingBuffer() : m_buf{}, m_head(0U), m_tail(0U)
    {
        NEVER_COMPILED_OUT_ASSERT(MSG_RING_CAPACITY > 0U);  // non-zero at construction
    }

    /// Initialise / reset the buffer.  Call during init phase only,
    /// before any concurrent access begins.
    void init()
    {
        NEVER_COMPILED_OUT_ASSERT(MSG_RING_CAPACITY > 0U);                                // non-zero
        NEVER_COMPILED_OUT_ASSERT((MSG_RING_CAPACITY & (MSG_RING_CAPACITY - 1U)) == 0U); // power of two
        // Relaxed: init() is called before concurrent access starts.
        m_head.store(0U, std::memory_order_relaxed);
        m_tail.store(0U, std::memory_order_relaxed);
    }


    // Safety-critical (SC): HAZ-006
    /**
     * @brief Push one envelope to the tail.
     * @return OK on success; ERR_FULL if capacity exhausted.
     * @note Call from producer thread only.
     *
     * Ordering: envelope_copy() writes data, then release store on m_tail
     * ensures the consumer's acquire load synchronises-with this store,
     * making all written data visible before pop() reads it.
     */
    Result push(const MessageEnvelope& env)
    {
        // Producer reads its own tail (relaxed) and consumer's head (acquire).
        const uint32_t t   = m_tail.load(std::memory_order_relaxed);
        const uint32_t h   = m_head.load(std::memory_order_acquire);
        const uint32_t cnt = t - h; // unsigned subtraction; correct on 32-bit wrap
        NEVER_COMPILED_OUT_ASSERT(cnt <= MSG_RING_CAPACITY);  // pre-condition: invariant holds

        if (cnt >= MSG_RING_CAPACITY) {
            return Result::ERR_FULL;
        }

        envelope_copy(m_buf[t & RING_MASK], env);

        // Release: all writes to m_buf complete before new tail is visible.
        m_tail.store(t + 1U, std::memory_order_release);

        NEVER_COMPILED_OUT_ASSERT((t + 1U - h) <= MSG_RING_CAPACITY);  // post-condition
        return Result::OK;
    }

    // Safety-critical (SC): HAZ-001, HAZ-004
    /**
     * @brief Pop one envelope from the head.
     * @param[out] env  Filled on success.
     * @return OK on success; ERR_EMPTY if the buffer is empty.
     * @note Call from consumer thread only.
     *
     * Ordering: acquire load of m_tail synchronises-with the producer's
     * release store, ensuring envelope_copy() sees fully written data.
     * After copying, release store on m_head lets the producer see the
     * freed slot via its acquire load of m_head.
     */
    Result pop(MessageEnvelope& env)
    {
        // Consumer reads its own head (relaxed) and producer's tail (acquire).
        const uint32_t h   = m_head.load(std::memory_order_relaxed);
        const uint32_t t   = m_tail.load(std::memory_order_acquire);
        const uint32_t cnt = t - h; // unsigned subtraction; correct on 32-bit wrap
        NEVER_COMPILED_OUT_ASSERT(cnt <= MSG_RING_CAPACITY);  // pre-condition: invariant holds

        if (cnt == 0U) {
            return Result::ERR_EMPTY;
        }

        envelope_copy(env, m_buf[h & RING_MASK]);

        // Release: envelope_copy complete before new head is visible to producer.
        m_head.store(h + 1U, std::memory_order_release);

        NEVER_COMPILED_OUT_ASSERT((t - (h + 1U)) < MSG_RING_CAPACITY);  // post-condition: count decreased
        return Result::OK;
    }

private:
    // Power of 10 rule 3: statically allocated storage, no dynamic allocation.
    MessageEnvelope              m_buf[MSG_RING_CAPACITY];

    // m_head: owned by consumer; stored with release, loaded by producer with acquire.
    // m_tail: owned by producer; stored with release, loaded by consumer with acquire.
    // F-Prime carve-out: std::atomic<uint32_t> is permitted for lock-free shared state.
    std::atomic<uint32_t>        m_head;
    std::atomic<uint32_t>        m_tail;
};

#endif // CORE_RING_BUFFER_HPP
