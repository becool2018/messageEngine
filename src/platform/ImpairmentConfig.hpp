/**
 * @file ImpairmentConfig.hpp
 * @brief Configuration structs for the impairment engine simulation layer.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §5 (Impairment engine requirements).
 *
 * Rules applied:
 *   - Power of 10 rule 3: all bounds are compile-time constants.
 *   - Power of 10 rule 8: configuration struct, no macros for control flow.
 *   - F-Prime style: plain POD struct, no STL, no templates.
 *
 * Usage:
 *   ImpairmentConfig cfg;
 *   impairment_config_default(cfg);
 *   cfg.fixed_latency_ms = 50;
 *   cfg.loss_probability = 0.05;
 *   // pass cfg to ImpairmentEngine::init()
 *
 * Implements: REQ-5.2.1, REQ-5.2.2, REQ-5.2.3, REQ-5.2.4
 */

#ifndef PLATFORM_IMPAIRMENT_CONFIG_HPP
#define PLATFORM_IMPAIRMENT_CONFIG_HPP

#include <cstdint>
#include "core/Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ImpairmentConfig: all configurable impairments for a channel or globally
// ─────────────────────────────────────────────────────────────────────────────

struct ImpairmentConfig {
    /// Master switch: if false, no impairments are applied.
    bool enabled;

    /// Fixed latency added to every message (milliseconds).
    /// 0 = disabled.
    uint32_t fixed_latency_ms;

    /// Mean and variance for random jitter (milliseconds).
    /// If jitter_mean_ms = 0, jitter is disabled.
    uint32_t jitter_mean_ms;
    uint32_t jitter_variance_ms;

    /// Probability [0.0, 1.0] that a message is dropped.
    /// 0.0 = no loss, 1.0 = all messages lost.
    double loss_probability;

    /// Probability [0.0, 1.0] that a message is duplicated.
    /// 0.0 = no duplication, 1.0 = all messages duplicated.
    double duplication_probability;

    /// Enable reordering: messages are buffered and released out of order.
    bool reorder_enabled;

    /// Maximum window size for reordering (≤ IMPAIR_DELAY_BUF_SIZE).
    /// If 0, reordering is disabled (even if reorder_enabled is true).
    uint32_t reorder_window_size;

    /// Enable partition / intermittent outage simulation.
    bool partition_enabled;

    /// Duration of each partition (no traffic) in milliseconds.
    uint32_t partition_duration_ms;

    /// Gap between partitions (traffic passes) in milliseconds.
    uint32_t partition_gap_ms;

    /// PRNG seed for deterministic impairment decisions.
    /// 0 = use default seed (42); non-zero = use provided seed.
    uint64_t prng_seed;
};

// ─────────────────────────────────────────────────────────────────────────────
// Inline default configuration function (Power of 10: inline not macros)
// ─────────────────────────────────────────────────────────────────────────────

/// Initialize an ImpairmentConfig to safe defaults (all impairments disabled).
inline void impairment_config_default(ImpairmentConfig& cfg)
{
    cfg.enabled                    = false;
    cfg.fixed_latency_ms           = 0U;
    cfg.jitter_mean_ms             = 0U;
    cfg.jitter_variance_ms         = 0U;
    cfg.loss_probability           = 0.0;
    cfg.duplication_probability    = 0.0;
    cfg.reorder_enabled            = false;
    cfg.reorder_window_size        = 0U;
    cfg.partition_enabled          = false;
    cfg.partition_duration_ms      = 0U;
    cfg.partition_gap_ms           = 0U;
    cfg.prng_seed                  = 42ULL;  // Default deterministic seed
}

#endif // PLATFORM_IMPAIRMENT_CONFIG_HPP
