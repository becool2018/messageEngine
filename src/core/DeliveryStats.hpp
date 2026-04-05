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
 * @file DeliveryStats.hpp
 * @brief Metrics structs for observability hooks (REQ-7.2.1–REQ-7.2.4).
 *
 * All counters use fixed-width unsigned integers; no dynamic allocation.
 * DeliveryEngine::get_stats() aggregates all sub-component stats into a
 * single DeliveryStats snapshot that callers can read without locking.
 *
 * Rules applied:
 *   - Power of 10 rule 3: no dynamic allocation; all fields are POD.
 *   - MISRA C++:2023: no STL, no templates.
 *   - F-Prime style: plain structs with explicit zero-init helper.
 *
 * Implements: REQ-7.2.1, REQ-7.2.2, REQ-7.2.3, REQ-7.2.4
 */
// Implements: REQ-7.2.1, REQ-7.2.2, REQ-7.2.3, REQ-7.2.4

#ifndef CORE_DELIVERY_STATS_HPP
#define CORE_DELIVERY_STATS_HPP

#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// ImpairmentStats — REQ-7.2.2
// ─────────────────────────────────────────────────────────────────────────────

/// Counters exported by ImpairmentEngine (REQ-7.2.2).
/// NSC: read-only observability; no effect on delivery.
struct ImpairmentStats {
    uint32_t loss_drops;        ///< Messages dropped by loss probability or partition
    uint32_t duplicate_injects; ///< Duplicate copies injected into delay buffer
    uint32_t partition_drops;   ///< Messages dropped specifically due to active partition
    uint32_t reorder_buffered;  ///< Messages that entered the reorder window buffer
};

// ─────────────────────────────────────────────────────────────────────────────
// RetryStats — REQ-7.2.3
// ─────────────────────────────────────────────────────────────────────────────

/// Counters exported by RetryManager (REQ-7.2.3).
/// NSC: read-only observability; no effect on delivery.
struct RetryStats {
    uint32_t retries_sent;    ///< Total retry transmissions (not counting first send)
    uint32_t slots_exhausted; ///< Slots that hit max_retries without ACK
    uint32_t slots_expired;   ///< Slots dropped because expiry_us passed before ACK
    uint32_t acks_received;   ///< ACKs that cancelled an active retry slot
};

// ─────────────────────────────────────────────────────────────────────────────
// AckTrackerStats — REQ-7.2.3
// ─────────────────────────────────────────────────────────────────────────────

/// Counters exported by AckTracker (REQ-7.2.3).
/// NSC: read-only observability; no effect on delivery.
struct AckTrackerStats {
    uint32_t timeouts;      ///< PENDING slots swept as expired (deadline passed)
    uint32_t acks_received; ///< PENDING→ACKED transitions recorded by on_ack()
};

// ─────────────────────────────────────────────────────────────────────────────
// TransportStats — REQ-7.2.4
// ─────────────────────────────────────────────────────────────────────────────

/// Counters exported by transport backends (REQ-7.2.4).
/// NSC: read-only observability; no effect on delivery.
struct TransportStats {
    uint32_t connections_opened; ///< Successful connect/accept events
    uint32_t connections_closed; ///< Disconnect events (graceful or abrupt)
    /// Impairment counters embedded here so DeliveryEngine::get_stats() can
    /// retrieve ImpairmentEngine data via the transport's get_transport_stats().
    ImpairmentStats impairment;
};

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryStats — aggregate snapshot (REQ-7.2.1–REQ-7.2.4)
// ─────────────────────────────────────────────────────────────────────────────

/// Aggregate metrics snapshot returned by DeliveryEngine::get_stats().
/// NSC: read-only observability; no effect on delivery.
struct DeliveryStats {
    // ── REQ-7.2.1 — latency (send→ACK round-trip) ────────────────────────────
    uint32_t latency_sample_count; ///< Number of RTT samples recorded
    uint64_t latency_sum_us;       ///< Sum of all RTT samples in microseconds
    uint64_t latency_min_us;       ///< Minimum RTT sample (0 if no samples yet)
    uint64_t latency_max_us;       ///< Maximum RTT sample (0 if no samples yet)

    // ── REQ-7.2.2 — impairment counters (from transport's ImpairmentEngine) ──
    ImpairmentStats impairment;

    // ── REQ-7.2.3 — retry / ACK / timeout ────────────────────────────────────
    RetryStats      retry;
    AckTrackerStats ack;

    // ── REQ-7.2.3 — message-level delivery counters ───────────────────────────
    uint32_t msgs_sent;               ///< Messages successfully submitted to transport
    uint32_t msgs_received;           ///< Data messages successfully delivered to caller
    uint32_t msgs_dropped_expired;    ///< Messages dropped because expiry_time_us passed
    uint32_t msgs_dropped_duplicate;  ///< Messages dropped by duplicate filter
    uint32_t msgs_dropped_misrouted;      ///< Messages dropped due to wrong destination_id
    uint32_t msgs_dropped_ordering_full;  ///< Messages dropped because ordering hold buffer was full (resource exhaustion, not routing error)

    // ── REQ-7.2.4 — connection / fatal event counters ────────────────────────
    TransportStats  transport;
    uint32_t        fatal_count;      ///< Fatal assertions fired since process start
};

// ─────────────────────────────────────────────────────────────────────────────
// delivery_stats_init() — zero all fields (Power of 10 rule 3: no heap)
// ─────────────────────────────────────────────────────────────────────────────

/// Zero-initialize a DeliveryStats struct.
/// Inline so callers in header-only contexts do not need a .cpp dependency.
inline void delivery_stats_init(DeliveryStats& s)
{
    (void)memset(&s, 0, sizeof(s));
}

/// Zero-initialize an ImpairmentStats struct.
inline void impairment_stats_init(ImpairmentStats& s)
{
    (void)memset(&s, 0, sizeof(s));
}

/// Zero-initialize a RetryStats struct.
inline void retry_stats_init(RetryStats& s)
{
    (void)memset(&s, 0, sizeof(s));
}

/// Zero-initialize an AckTrackerStats struct.
inline void ack_tracker_stats_init(AckTrackerStats& s)
{
    (void)memset(&s, 0, sizeof(s));
}

/// Zero-initialize a TransportStats struct.
inline void transport_stats_init(TransportStats& s)
{
    (void)memset(&s, 0, sizeof(s));
}

#endif // CORE_DELIVERY_STATS_HPP
