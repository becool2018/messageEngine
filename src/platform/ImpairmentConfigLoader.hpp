/**
 * @file ImpairmentConfigLoader.hpp
 * @brief Load an ImpairmentConfig from an INI-style key=value text file.
 *
 * File format:
 *   - One key=value pair per line; whitespace around '=' is optional.
 *   - Lines beginning with '#' or ';' are comments and are ignored.
 *   - Blank lines are ignored.
 *   - Unknown keys are logged at WARNING_LO and ignored.
 *   - Missing keys retain their default values (impairment_config_default()).
 *   - At most MAX_CONFIG_LINES lines are processed; lines beyond that limit
 *     are silently ignored (bounded loop — Power of 10 Rule 2).
 *
 * Example file:
 *   enabled = 1
 *   fixed_latency_ms = 50
 *   loss_probability = 0.05
 *   prng_seed = 42
 *
 * Supported keys:
 *   enabled, fixed_latency_ms, jitter_mean_ms, jitter_variance_ms,
 *   loss_probability, duplication_probability,
 *   reorder_enabled, reorder_window_size,
 *   partition_enabled, partition_duration_ms, partition_gap_ms,
 *   prng_seed
 *
 * Rules applied:
 *   - Power of 10 Rule 2: bounded loop at most MAX_CONFIG_LINES iterations.
 *   - Power of 10 Rule 3: fixed stack buffer; no heap allocation.
 *   - Power of 10 Rule 5: ≥2 assertions per function.
 *   - Power of 10 Rule 7: all return values checked.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: Result return code; Logger::log() for events.
 *
 * Implements: REQ-5.2.1
 */

#ifndef PLATFORM_IMPAIRMENT_CONFIG_LOADER_HPP
#define PLATFORM_IMPAIRMENT_CONFIG_LOADER_HPP

#include "core/ImpairmentConfig.hpp"
#include "core/Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Capacity constant — exposed so callers can document the loop bound
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum number of lines read from a config file (Power of 10 Rule 2).
static const uint32_t MAX_CONFIG_LINES = 64U;

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Load an ImpairmentConfig from a plain-text INI-style file.
 *
 * @param[in]  path  Null-terminated path to the config file.
 * @param[out] cfg   Config struct to populate.
 *                   Always initialized to safe defaults via
 *                   impairment_config_default() before parsing begins,
 *                   even when ERR_IO is returned.
 *
 * @return Result::OK      — file opened and parsed (zero or more keys loaded).
 * @return Result::ERR_IO  — file could not be opened; cfg contains defaults.
 *
 * Safety-critical (SC): HAZ-002, HAZ-007
 */
Result impairment_config_load(const char* path, ImpairmentConfig& cfg);

#endif // PLATFORM_IMPAIRMENT_CONFIG_LOADER_HPP
