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
 * @file ImpairmentEngine.cpp
 * @brief Implementation of the ImpairmentEngine class.
 *
 * Applies network impairments (loss, duplication, latency, jitter, reordering, partitions)
 * to MessageEnvelope objects in a deterministic manner controlled by a seedable PRNG.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds using IMPAIR_DELAY_BUF_SIZE, no recursion,
 *     ≥2 assertions per method, all return values checked.
 *   - MISRA C++: no STL, no exceptions, no templates.
 *   - F-Prime style: Result enum return codes, Logger::log() for events.
 *
 * Implements: REQ-5.1.1, REQ-5.1.2, REQ-5.1.3, REQ-5.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.2.2, REQ-5.2.4, REQ-5.2.5, REQ-5.3.1, REQ-5.3.2, REQ-7.2.2
 */

#include "ImpairmentEngine.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <sys/random.h>
#endif

// CERT / REQ-5.2.4: Prevent ALLOW_WEAK_PRNG_SEED from being compiled into release builds.
// NDEBUG is set by production/release build profiles; a weak seed in production violates
// REQ-5.2.4 (cryptographically unpredictable source required).
#if defined(ALLOW_WEAK_PRNG_SEED) && defined(NDEBUG)
#error "ALLOW_WEAK_PRNG_SEED must not be defined in release/production builds (REQ-5.2.4)"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

ImpairmentEngine::ImpairmentEngine()
    : m_cfg{}, m_delay_count(0U), m_reorder_count(0U),
      m_partition_active(false), m_partition_start_us(0ULL),
      m_next_partition_event_us(0ULL), m_initialized(false), m_stats{}
{
    // Power of 10 rule 3: zero all statically allocated buffers
    (void)memset(m_delay_buf, 0, sizeof(m_delay_buf));
    (void)memset(m_reorder_buf, 0, sizeof(m_reorder_buf));
    // Seed placeholder — init() must be called before use; do not use PRNG before init().
    // REQ-5.2.4: 0 is an explicit sentinel meaning "not yet seeded"; init() will reseed
    // with cryptographic entropy (or the caller-supplied test seed from cfg.prng_seed).
    m_prng.seed(0ULL);
    NEVER_COMPILED_OUT_ASSERT(!m_initialized);          // Power of 10: precondition
    NEVER_COMPILED_OUT_ASSERT(IMPAIR_DELAY_BUF_SIZE > 0U);  // Power of 10: bounds check
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

void ImpairmentEngine::init(const ImpairmentConfig& cfg)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE);
    NEVER_COMPILED_OUT_ASSERT(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0);
    // MED-7 fix: partition_gap_ms==0 with partition enabled causes an immediate, permanent
    // partition on the very first call to is_partition_active() (documented in
    // STATE_MACHINES.md §3 "Known edge case"). Reject this at init time so the
    // configuration error is detected immediately rather than causing silent message loss.
    NEVER_COMPILED_OUT_ASSERT(!cfg.partition_enabled || cfg.partition_gap_ms > 0U);

    // Store configuration
    m_cfg = cfg;

    // Implements: REQ-5.2.4
    // Seed PRNG: if cfg.prng_seed is non-zero, use it directly (test/deterministic mode).
    // If cfg.prng_seed == 0, gather cryptographic entropy from the OS so that the seed
    // cannot be predicted from a known message sequence (SECURITY_ASSUMPTIONS.md §7).
    uint64_t seed = 0U;
    if (cfg.prng_seed != 0ULL) {
        // REQ-5.2.4: caller-supplied deterministic seed (test mode); non-zero means explicit.
        seed = cfg.prng_seed;
    } else {
        // REQ-5.2.4: seed from cryptographically unpredictable entropy source.
        uint64_t entropy = 0U;
#if defined(__APPLE__)
        // macOS: arc4random_buf() is always available and uses the kernel CSPRNG; no include needed.
        arc4random_buf(&entropy, sizeof(entropy));
#else
        // Linux / POSIX: getrandom() — requires <sys/random.h> (included above).
        // MISRA 5.2.4 deviation: reinterpret_cast to void* is required by the getrandom()
        // POSIX API signature; no safer cast exists for this OS call.
        // CERT INT32-C: getrandom() returns ssize_t; compare against cast sizeof to avoid
        // signed/unsigned mismatch warning (-Wsign-compare).
        // Tier 1: getrandom() — preferred; direct kernel CSPRNG.
        {
            ssize_t got = getrandom(
                static_cast<void*>(&entropy),       // MISRA 5.2.4: void* required by POSIX API
                sizeof(entropy), 0U);
            if (got != static_cast<ssize_t>(sizeof(entropy))) {
                // Tier 2: /dev/urandom — available even early-boot when getrandom() blocks.
                // SECfix-4: adds /dev/urandom tier missing from prior two-tier chain.
                Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                            "getrandom() failed (got=%ld); trying /dev/urandom",
                            static_cast<long>(got));
                int urfd = open("/dev/urandom", O_RDONLY);
                if (urfd >= 0) {
                    ssize_t r = read(urfd, static_cast<void*>(&entropy), sizeof(entropy));
                    (void)close(urfd);
                    if (r != static_cast<ssize_t>(sizeof(entropy))) {
                        entropy = 0ULL;  // Mark as failed; tier 3 will handle it.
                    }
                } else {
                    entropy = 0ULL;  // Mark as failed; tier 3 will handle it.
                }
            }
        }
#endif
        // Guard: entropy all-zero after OS gather is astronomically unlikely but possible.
        // Mix additional sources rather than fall back to a known literal (REQ-5.2.4).
        if (entropy == 0ULL) {
            // F-8 / REQ-5.2.4: OS entropy sources (getrandom, /dev/urandom) both
            // returned zero. A clock()+pid fallback seed is predictable (~30 bits,
            // time-based) and is explicitly prohibited in production by REQ-5.2.4.
            // In production builds, fail initialization rather than proceed with a
            // guessable seed. Set ALLOW_WEAK_PRNG_SEED only in non-production
            // environments (embedded dev boards, CI without /dev/urandom).
#if defined(ALLOW_WEAK_PRNG_SEED)
            entropy = (static_cast<uint64_t>(clock()) << 32U)
                      ^ static_cast<uint64_t>(getpid());
            if (entropy == 0ULL) { entropy = 1ULL; }
            Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                        "OS entropy was zero; using clock/pid fallback — "
                        "NOT PRODUCTION SAFE (ALLOW_WEAK_PRNG_SEED defined)");
#else
            // Production: treat entropy exhaustion as a platform fault. Log FATAL
            // and return without setting m_initialized; callers will hit their
            // NEVER_COMPILED_OUT_ASSERT(m_initialized) guards.
            Logger::log(Severity::FATAL, "ImpairmentEngine",
                        "OS entropy sources exhausted; cannot seed PRNG securely "
                        "(REQ-5.2.4). Build with -DALLOW_WEAK_PRNG_SEED to override "
                        "in non-production environments.");
            NEVER_COMPILED_OUT_ASSERT(false);  // Assert: unreachable in production
            return;
#endif
        }
        // REQ-5.2.4: no known-constant fallback
        seed = entropy;
    }
    m_prng.seed(seed);

    // Zero-fill delay buffer (Power of 10: memset with fixed bounds)
    (void)memset(m_delay_buf, 0, sizeof(m_delay_buf));
    m_delay_count = 0U;

    // Zero-fill reorder buffer
    (void)memset(m_reorder_buf, 0, sizeof(m_reorder_buf));
    m_reorder_count = 0U;

    // Initialize partition state
    m_partition_active = false;
    m_partition_start_us = 0ULL;
    // First call to is_partition_active() will initialize m_next_partition_event_us
    m_next_partition_event_us = 0ULL;

    // Mark as initialized
    m_initialized = true;

    // REQ-7.2.2: zero all observability counters on (re-)init
    impairment_stats_init(m_stats);

    // Power of 10 rule 5: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(m_delay_count == 0U);
    NEVER_COMPILED_OUT_ASSERT(m_stats.loss_drops == 0U);  // Assert: stats zeroed
}

// ─────────────────────────────────────────────────────────────────────────────
// queue_to_delay_buf() — private helper
// ─────────────────────────────────────────────────────────────────────────────

Result ImpairmentEngine::queue_to_delay_buf(const MessageEnvelope& env,
                                             uint64_t release_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                           // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE);  // Power of 10: slot available

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < IMPAIR_DELAY_BUF_SIZE; ++i) {
        if (!m_delay_buf[i].active) {
            envelope_copy(m_delay_buf[i].env, env);
            m_delay_buf[i].release_us = release_us;
            m_delay_buf[i].active = true;
            ++m_delay_count;
            NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);  // Power of 10: postcondition
            return Result::OK;
        }
    }

    // Should never reach here: caller verified m_delay_count < buf size
    NEVER_COMPILED_OUT_ASSERT(false);  // Logic error: no free slot despite count check
    return Result::ERR_FULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// check_loss() — private helper
// ─────────────────────────────────────────────────────────────────────────────

bool ImpairmentEngine::check_loss()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                      // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_cfg.loss_probability <= 1.0);      // Power of 10: valid probability

    if (m_cfg.loss_probability <= 0.0) {
        return false;
    }
    double rand_val = m_prng.next_double();
    NEVER_COMPILED_OUT_ASSERT(rand_val >= 0.0 && rand_val < 1.0);  // Power of 10: valid random value
    return rand_val < m_cfg.loss_probability;
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_duplication() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void ImpairmentEngine::apply_duplication(const MessageEnvelope& env,
                                          uint64_t release_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                           // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_cfg.duplication_probability > 0.0);    // Power of 10: called only when enabled

    double dup_rand = m_prng.next_double();
    NEVER_COMPILED_OUT_ASSERT(dup_rand >= 0.0 && dup_rand < 1.0);      // Power of 10: valid random value

    if (dup_rand < m_cfg.duplication_probability) {
        if (m_delay_count < IMPAIR_DELAY_BUF_SIZE) {
            Result res = queue_to_delay_buf(env, release_us + 100ULL);
            if (result_ok(res)) {
                ++m_stats.duplicate_injects;  // REQ-7.2.2: record duplicate injection
                Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                           "message duplicated");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_jitter_us() — private helper
// ─────────────────────────────────────────────────────────────────────────────

uint64_t ImpairmentEngine::compute_jitter_us()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Power of 10: initialized

    if (m_cfg.jitter_mean_ms == 0U) {
        NEVER_COMPILED_OUT_ASSERT(m_cfg.jitter_mean_ms == 0U);  // Power of 10: guard confirmed
        return 0ULL;
    }

    // REQ-5.1.2: bidirectional jitter centered on mean_ms with ±variance_ms range.
    // When variance_ms <= mean_ms: range = [mean - variance, mean + variance].
    // When variance_ms > mean_ms: lower bound clamped to 0 to avoid uint underflow;
    //   effective range = [0, mean + variance].
    // hi_ms >= lo_ms by construction; next_range(lo, hi) returns [lo, hi] inclusive.
    uint32_t lo_ms = (m_cfg.jitter_variance_ms <= m_cfg.jitter_mean_ms)
        ? (m_cfg.jitter_mean_ms - m_cfg.jitter_variance_ms)
        : 0U;
    // CERT INT30-C: guard uint32_t addition overflow before computing hi_ms (S1).
    // Clamp to UINT32_MAX-1 (not UINT32_MAX) so that next_range(lo_ms, hi_ms)
    // can never produce range = hi-lo+1 = 0 via uint32 wrap (PrngEngine line 140).
    uint32_t hi_ms = 0U;
    if (m_cfg.jitter_variance_ms > (UINT32_MAX - m_cfg.jitter_mean_ms)) {
        Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                    "jitter overflow: mean=%u variance=%u; clamping hi to UINT32_MAX-1",
                    m_cfg.jitter_mean_ms, m_cfg.jitter_variance_ms);
        hi_ms = UINT32_MAX - 1U;
    } else {
        hi_ms = m_cfg.jitter_mean_ms + m_cfg.jitter_variance_ms;
    }

    NEVER_COMPILED_OUT_ASSERT(hi_ms >= lo_ms);  // Power of 10: range is non-negative

    uint32_t jitter_offset_ms = m_prng.next_range(lo_ms, hi_ms);
    return static_cast<uint64_t>(jitter_offset_ms) * 1000ULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_release_us() — private helper
// ─────────────────────────────────────────────────────────────────────────────

uint64_t ImpairmentEngine::compute_release_us(uint64_t now_us,
                                               uint64_t base_delay_us,
                                               uint64_t jitter_us)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(base_delay_us <= UINT64_MAX / 2ULL);  // Power of 10: sane delay

    // CERT INT30-C: guard three-term uint64_t addition against overflow.
    // Cap each intermediate sum at UINT64_MAX/2 to prevent silent wrap-around.
    // (SECfix-6: three-term addition overflow extracted from process_outbound)
    static const uint64_t k_release_cap_us = UINT64_MAX / 2ULL;
    uint64_t delay_total_us = 0ULL;
    if (base_delay_us > k_release_cap_us - jitter_us) {
        Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                    "delay overflow: base=%llu jitter=%llu; clamping to cap",
                    static_cast<unsigned long long>(base_delay_us),
                    static_cast<unsigned long long>(jitter_us));
        delay_total_us = k_release_cap_us;
    } else {
        delay_total_us = base_delay_us + jitter_us;
    }
    uint64_t release_us = 0ULL;
    if (now_us > UINT64_MAX - delay_total_us) {
        Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                    "release_us overflow: now=%llu delay=%llu; clamping to UINT64_MAX",
                    static_cast<unsigned long long>(now_us),
                    static_cast<unsigned long long>(delay_total_us));
        release_us = UINT64_MAX;
    } else {
        release_us = now_us + delay_total_us;
    }
    // Power of 10 rule 5: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(release_us >= now_us || release_us == UINT64_MAX);
    return release_us;
}

// ─────────────────────────────────────────────────────────────────────────────
// check_delay_buf_watermark() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void ImpairmentEngine::check_delay_buf_watermark() const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                              // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);    // Power of 10: valid count

    // Early-warning: log when delay buffer approaches capacity (>80%).
    // Prevents silent ERR_FULL surprise under sustained high-latency impairment.
    // Threshold: (IMPAIR_DELAY_BUF_SIZE * 80U) / 100U = 25 slots for size=32.
    static const uint32_t k_delay_warn_threshold =
        (IMPAIR_DELAY_BUF_SIZE * 80U) / 100U;

    if (m_delay_count > k_delay_warn_threshold) {
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                    "delay buffer approaching capacity: %u/%u slots used",
                    static_cast<unsigned>(m_delay_count),
                    static_cast<unsigned>(IMPAIR_DELAY_BUF_SIZE));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// process_outbound()
// ─────────────────────────────────────────────────────────────────────────────

Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env,
                                          uint64_t now_us)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env));

    // If impairments disabled, pass through immediately with zero latency
    if (!m_cfg.enabled) {
        if (m_delay_count >= IMPAIR_DELAY_BUF_SIZE) {
            Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                       "delay buffer full (disabled impairments)");
            return Result::ERR_FULL;
        }
        Result r_passthrough = queue_to_delay_buf(in_env, now_us);
        if (result_ok(r_passthrough)) {
            check_delay_buf_watermark();  // REQ-7.2.2: early-warning on 80% capacity
        }
        return r_passthrough;
    }

    // Drop if partition is active
    if (is_partition_active(now_us)) {
        ++m_stats.partition_drops;  // REQ-7.2.2: partition-specific drop
        ++m_stats.loss_drops;       // REQ-7.2.2: total loss drop (partition counts as loss)
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "message dropped (partition active)");
        return Result::ERR_IO;
    }

    // Drop if loss impairment fires
    if (check_loss()) {
        ++m_stats.loss_drops;  // REQ-7.2.2: probabilistic loss drop
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "message dropped (loss probability)");
        return Result::ERR_IO;
    }

    // Calculate release time: fixed latency + jitter (REQ-5.1.1, REQ-5.1.2)
    // CERT INT30-C overflow guards are inside compute_release_us() (SECfix-6).
    uint64_t base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL;
    uint64_t jitter_us = compute_jitter_us();
    uint64_t release_us = compute_release_us(now_us, base_delay_us, jitter_us);

    // Queue the message in the delay buffer
    if (m_delay_count >= IMPAIR_DELAY_BUF_SIZE) {
        Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                   "delay buffer full; dropping message");
        return Result::ERR_FULL;
    }
    Result res = queue_to_delay_buf(in_env, release_us);
    if (!result_ok(res)) {
        return res;
    }
    check_delay_buf_watermark();  // REQ-7.2.2: early-warning on 80% capacity

    // Probabilistically queue a duplicate copy
    if (m_cfg.duplication_probability > 0.0) {
        apply_duplication(in_env, release_us);
    }

    // Power of 10 rule 5: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// collect_deliverable()
// ─────────────────────────────────────────────────────────────────────────────

uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us,
                                                MessageEnvelope* out_buf,
                                                uint32_t buf_cap)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U);

    uint32_t out_count = 0U;

    // Power of 10: fixed loop bound using IMPAIR_DELAY_BUF_SIZE
    for (uint32_t i = 0U; i < IMPAIR_DELAY_BUF_SIZE && out_count < buf_cap; ++i) {
        if (m_delay_buf[i].active && m_delay_buf[i].release_us <= now_us) {
            // Message is ready for delivery
            envelope_copy(out_buf[out_count], m_delay_buf[i].env);
            ++out_count;

            // Mark slot as inactive and decrement count
            m_delay_buf[i].active = false;
            NEVER_COMPILED_OUT_ASSERT(m_delay_count > 0U);
            --m_delay_count;
        }
    }

    // Power of 10 rule 5: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);
    return out_count;
}

// ─────────────────────────────────────────────────────────────────────────────
// process_inbound()
// ─────────────────────────────────────────────────────────────────────────────

// cppcheck-suppress unusedFunction -- called from tests/test_ImpairmentEngine.cpp
Result ImpairmentEngine::process_inbound(const MessageEnvelope& in_env,
                                        uint64_t now_us,
                                        MessageEnvelope* out_buf,
                                        uint32_t buf_cap,
                                        uint32_t& out_count)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env));
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U);

    // Initialize output count
    out_count = 0U;

    // REQ-5.1.6: drop inbound messages if partition is active.
    if (m_cfg.enabled && is_partition_active(now_us)) {
        ++m_stats.partition_drops;  // REQ-7.2.2: partition-specific drop
        ++m_stats.loss_drops;       // REQ-7.2.2: total loss drop (partition counts as loss)
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "inbound message dropped (partition active)");
        NEVER_COMPILED_OUT_ASSERT(out_count == 0U);  // Power of 10: no output on drop
        return Result::ERR_IO;
    }

    // If reordering disabled or window size is 0, pass through immediately
    if (!m_cfg.reorder_enabled || m_cfg.reorder_window_size == 0U) {
        envelope_copy(out_buf[0], in_env);
        out_count = 1U;
        NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap);
        return Result::OK;
    }

    // Reordering is enabled; buffer the message if window not full
    if (m_reorder_count < m_cfg.reorder_window_size) {
        // Buffer the new message
        envelope_copy(m_reorder_buf[m_reorder_count], in_env);
        ++m_reorder_count;
        ++m_stats.reorder_buffered;  // REQ-7.2.2: record reorder buffer entry
        // Do not deliver yet; wait until window fills or timeout
        out_count = 0U;
        NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE);
        return Result::OK;
    }

    // Window is full; randomly release one buffered message and add the new one
    // Power of 10: fixed loop bound
    uint32_t release_idx = m_prng.next_range(0U, m_reorder_count - 1U);
    NEVER_COMPILED_OUT_ASSERT(release_idx < m_reorder_count);

    if (buf_cap >= 1U) {
        // Release the randomly selected message
        envelope_copy(out_buf[0], m_reorder_buf[release_idx]);
        out_count = 1U;

        // Remove the released message from the window by shifting
        // (Power of 10: fixed loop bound)
        for (uint32_t i = release_idx; i < m_reorder_count - 1U; ++i) {
            envelope_copy(m_reorder_buf[i], m_reorder_buf[i + 1U]);
        }
        --m_reorder_count;

        // Add the new message to the back of the window
        if (m_reorder_count < m_cfg.reorder_window_size) {
            envelope_copy(m_reorder_buf[m_reorder_count], in_env);
            ++m_reorder_count;
        }
    }

    // Power of 10 rule 5: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= m_cfg.reorder_window_size);
    NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// sat_add_us() — saturating uint64 addition helper (CERT INT30-C)
// Returns now_us + delta_ms*1000, saturating at UINT64_MAX instead of wrapping.
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t sat_add_us(uint64_t now_us, uint32_t delta_ms)
{
    const uint64_t k_max   = 0xFFFFFFFFFFFFFFFFULL;
    const uint64_t delta   = static_cast<uint64_t>(delta_ms) * 1000ULL;
    NEVER_COMPILED_OUT_ASSERT(delta_ms <= 0xFFFFFFFFUL);   // Pre: fits uint32
    return (now_us > k_max - delta) ? k_max : now_us + delta;
}

// ─────────────────────────────────────────────────────────────────────────────
// is_partition_active()
// ─────────────────────────────────────────────────────────────────────────────

bool ImpairmentEngine::is_partition_active(uint64_t now_us)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);

    // If partitions not enabled, return false
    if (!m_cfg.partition_enabled) {
        return false;
    }

    // Initialize partition event timing on first call
    if (m_next_partition_event_us == 0ULL) {
        // Start with gap (no partition yet)
        m_next_partition_event_us = sat_add_us(now_us, m_cfg.partition_gap_ms);  // CERT INT30-C
        m_partition_active = false;
        NEVER_COMPILED_OUT_ASSERT(!m_partition_active);
        return false;
    }

    // State machine: transition if event time reached
    if (!m_partition_active && now_us >= m_next_partition_event_us) {
        // Start a partition
        m_partition_active = true;
        m_partition_start_us = now_us;
        m_next_partition_event_us = sat_add_us(now_us, m_cfg.partition_duration_ms);  // CERT INT30-C
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "partition started (duration: %u ms)",
                   m_cfg.partition_duration_ms);
        NEVER_COMPILED_OUT_ASSERT(m_partition_active);
        return true;
    }

    if (m_partition_active && now_us >= m_next_partition_event_us) {
        // End the partition
        m_partition_active = false;
        m_next_partition_event_us = sat_add_us(now_us, m_cfg.partition_gap_ms);  // CERT INT30-C
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "partition ended");
        NEVER_COMPILED_OUT_ASSERT(!m_partition_active);
        return false;
    }

    // Power of 10 rule 5: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL);
    return m_partition_active;
}

// ─────────────────────────────────────────────────────────────────────────────
// ImpairmentEngine::get_stats() — REQ-7.2.2 observability accessor
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

const ImpairmentStats& ImpairmentEngine::get_stats() const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine was initialized
    NEVER_COMPILED_OUT_ASSERT(m_stats.loss_drops >= m_stats.partition_drops);  // Assert: partition subset of loss
    return m_stats;
}
