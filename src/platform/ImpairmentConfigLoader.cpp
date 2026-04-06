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
 * @file ImpairmentConfigLoader.cpp
 * @brief Implementation of impairment_config_load(): INI-style config file parser.
 *
 * Rules applied:
 *   - Power of 10 Rule 2: bounded loop — at most MAX_CONFIG_LINES fgets calls.
 *   - Power of 10 Rule 3: fixed stack buffers only; no heap allocation.
 *   - Power of 10 Rule 4: short, single-purpose functions (CC ≤ 10 each).
 *   - Power of 10 Rule 5: ≥2 assertions per function.
 *   - Power of 10 Rule 7: every non-void return value checked.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: Result return code; Logger::log() for events.
 *
 * Implements: REQ-5.2.1
 */

#include "ImpairmentConfigLoader.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include <cerrno>
#include <cmath>    // std::isnan, std::isinf — L-4: NaN/Inf guard after strtod()
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// File-local constants (Power of 10 Rule 2 / Rule 8)
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum characters per input line, including newline and NUL terminator.
static const uint32_t MAX_CONFIG_LINE_LEN = 128U;

// ─────────────────────────────────────────────────────────────────────────────
// Leaf parse helpers — strip nesting from apply_kv branches (CC-reduction)
// Use strto* (not sscanf) to satisfy bugprone-unchecked-string-to-number-conversion:
// check the end pointer to confirm the entire token was consumed.
// Each returns true on successful parse, false on failure (field unchanged).
// ─────────────────────────────────────────────────────────────────────────────

/// Parse a uint32 from @p val into @p out. Returns true on success.
/// Security: sets errno=0 before strtoul and checks ERANGE and UINT32_MAX
/// to prevent silent truncation on 64-bit platforms where unsigned long > 32 bits.
static bool parse_uint(const char* val, uint32_t* out)
{
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out != nullptr);
    char* end = nullptr;
    errno = 0;
    unsigned long v = strtoul(val, &end, 10);
    if (end == val || *end != '\0') { return false; }
    if (errno == ERANGE) { return false; }
    // Guard against 64-bit unsigned long holding a value > UINT32_MAX
    if (v > static_cast<unsigned long>(0xFFFFFFFFUL)) { return false; }
    *out = static_cast<uint32_t>(v);
    return true;
}

/// Parse a uint32 from @p val and store as bool in @p out. Returns true on success.
static bool parse_bool(const char* val, bool* out)
{
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out != nullptr);
    char* end = nullptr;
    unsigned long v = strtoul(val, &end, 10);
    if (end == val || *end != '\0') { return false; }
    *out = (v != 0UL);
    return true;
}

/// Parse a double from @p val, clamp to [0.0, 1.0], store in @p out.
/// Returns true on success.
static bool parse_prob(const char* val, double* out)
{
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out != nullptr);
    char* end = nullptr;
    double v = strtod(val, &end);
    if (end == val || *end != '\0') { return false; }
    // L-4: Guard against NaN and Inf which strtod() can return without setting
    // ERANGE (e.g., "nan" or "inf" inputs).  std::isnan / std::isinf from <cmath>
    // are used here; they introduce no dynamic allocation and are MISRA-safe.
    if (std::isnan(v) || std::isinf(v)) { return false; }
    // Clamp to [0.0, 1.0] (Power of 10: explicit bounds check)
    if (v < 0.0) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "Probability %.6f < 0.0; clamped to 0.0", v);
        v = 0.0;
    }
    if (v > 1.0) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "Probability %.6f > 1.0; clamped to 1.0", v);
        v = 1.0;
    }
    *out = v;
    NEVER_COMPILED_OUT_ASSERT(*out >= 0.0 && *out <= 1.0);
    return true;
}

/// Parse a uint64 from @p val into @p out. Returns true on success.
/// Security: sets errno=0 before strtoull and checks ERANGE after the call.
static bool parse_u64(const char* val, uint64_t* out)
{
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out != nullptr);
    char* end = nullptr;
    errno = 0;
    // strtoull returns unsigned long long; safe cast to uint64_t (both ≥ 64 bits).
    unsigned long long v = strtoull(val, &end, 10);  // NOLINT(google-runtime-int)
    if (end == val || *end != '\0') { return false; }
    if (errno == ERANGE) { return false; }
    *out = static_cast<uint64_t>(v);
    return true;
}

/// Parse and apply reorder_window_size: parse uint32, clamp to IMPAIR_DELAY_BUF_SIZE.
/// Security: sets errno=0 before strtoul and checks ERANGE and UINT32_MAX before cast.
static void apply_reorder_window(const char* val, ImpairmentConfig& cfg)
{
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    char* end = nullptr;
    errno = 0;
    unsigned long v = strtoul(val, &end, 10);
    if (end == val || *end != '\0') { return; }
    if (errno == ERANGE || v > static_cast<unsigned long>(0xFFFFFFFFUL)) { return; }
    uint32_t w = static_cast<uint32_t>(v);
    // Clamp to IMPAIR_DELAY_BUF_SIZE (Power of 10: bounded buffer)
    if (w > IMPAIR_DELAY_BUF_SIZE) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "reorder_window_size %u exceeds IMPAIR_DELAY_BUF_SIZE %u; clamping",
                    w, IMPAIR_DELAY_BUF_SIZE);
        w = IMPAIR_DELAY_BUF_SIZE;
    }
    cfg.reorder_window_size = w;
    NEVER_COMPILED_OUT_ASSERT(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Topic dispatchers — each handles one semantic group of 4 keys.
// Returns true if the key was recognised and handled, false otherwise.
// ─────────────────────────────────────────────────────────────────────────────

/// Handle: enabled, fixed_latency_ms, jitter_mean_ms, jitter_variance_ms.
static bool apply_kv_latency_jitter(const char* key, const char* val,
                                     ImpairmentConfig& cfg)
{
    NEVER_COMPILED_OUT_ASSERT(key != nullptr);
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    if (strcmp(key, "enabled") == 0) {
        (void)parse_bool(val, &cfg.enabled);
    } else if (strcmp(key, "fixed_latency_ms") == 0) {
        (void)parse_uint(val, &cfg.fixed_latency_ms);
    } else if (strcmp(key, "jitter_mean_ms") == 0) {
        (void)parse_uint(val, &cfg.jitter_mean_ms);
    } else if (strcmp(key, "jitter_variance_ms") == 0) {
        (void)parse_uint(val, &cfg.jitter_variance_ms);
    } else {
        return false;
    }
    return true;
}

/// Handle: loss_probability, duplication_probability, reorder_enabled, reorder_window_size.
static bool apply_kv_loss_reorder(const char* key, const char* val,
                                   ImpairmentConfig& cfg)
{
    NEVER_COMPILED_OUT_ASSERT(key != nullptr);
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    if (strcmp(key, "loss_probability") == 0) {
        (void)parse_prob(val, &cfg.loss_probability);
    } else if (strcmp(key, "duplication_probability") == 0) {
        (void)parse_prob(val, &cfg.duplication_probability);
    } else if (strcmp(key, "reorder_enabled") == 0) {
        (void)parse_bool(val, &cfg.reorder_enabled);
    } else if (strcmp(key, "reorder_window_size") == 0) {
        apply_reorder_window(val, cfg);
    } else {
        return false;
    }
    return true;
}

/// Handle: partition_enabled, partition_duration_ms, partition_gap_ms, prng_seed.
static bool apply_kv_partition_seed(const char* key, const char* val,
                                     ImpairmentConfig& cfg)
{
    NEVER_COMPILED_OUT_ASSERT(key != nullptr);
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    if (strcmp(key, "partition_enabled") == 0) {
        (void)parse_bool(val, &cfg.partition_enabled);
    } else if (strcmp(key, "partition_duration_ms") == 0) {
        (void)parse_uint(val, &cfg.partition_duration_ms);
    } else if (strcmp(key, "partition_gap_ms") == 0) {
        (void)parse_uint(val, &cfg.partition_gap_ms);
    } else if (strcmp(key, "prng_seed") == 0) {
        (void)parse_u64(val, &cfg.prng_seed);
    } else {
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper — apply one parsed key/value pair to cfg
// ─────────────────────────────────────────────────────────────────────────────

/// Apply a single key/value string pair to the ImpairmentConfig struct.
/// Dispatches to topic helpers; logs unknown keys at WARNING_LO.
static void apply_kv(const char* key, const char* val, ImpairmentConfig& cfg)
{
    NEVER_COMPILED_OUT_ASSERT(key != nullptr);
    NEVER_COMPILED_OUT_ASSERT(val != nullptr);
    bool handled = apply_kv_latency_jitter(key, val, cfg);
    if (!handled) { handled = apply_kv_loss_reorder(key, val, cfg); }
    if (!handled) { handled = apply_kv_partition_seed(key, val, cfg); }
    if (!handled) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "Unknown config key ignored: %.40s", key);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Static helper — parse one text line and update cfg if it contains a key=value
// ─────────────────────────────────────────────────────────────────────────────

/// Parse a single input line. If the line contains a valid key=value pair,
/// delegate to apply_kv(). Blank lines and comment lines (#, ;) are silently
/// skipped. Malformed lines are logged at WARNING_LO.
static void parse_config_line(const char* line, ImpairmentConfig& cfg)
{
    NEVER_COMPILED_OUT_ASSERT(line != nullptr);

    // Skip leading whitespace
    const char* p = line;
    while (*p == ' ' || *p == '\t') { ++p; }

    // Skip blank lines and comment lines
    if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '\r') {
        return;
    }

    // Extract key and value using sscanf.
    // Format: "%63[^ \t=]%*[ \t=]%63s"
    //   %63[^ \t=]  — scan key: characters that are not space, tab, or '='
    //   %*[ \t=]    — discard separator (one or more of: space, tab, '=')
    //   %63s         — scan value: first non-whitespace token
    // Both key and val are zero-initialised before scanning.
    // Power of 10 Rule 3: fixed-size stack buffers.
    char key[64];
    char val[64];
    (void)memset(key, 0, sizeof(key));
    (void)memset(val, 0, sizeof(val));

    int n = sscanf(p, "%63[^ \t=]%*[ \t=]%63s", key, val);
    if (n != 2) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "Skipping malformed config line: %.60s", p);
        return;
    }

    // Postcondition: sscanf matched both tokens; key is non-empty.
    NEVER_COMPILED_OUT_ASSERT(key[0] != '\0');

    apply_kv(key, val, cfg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public function
// ─────────────────────────────────────────────────────────────────────────────

Result impairment_config_load(const char* path, ImpairmentConfig& cfg)
{
    // Precondition: path must not be null.
    NEVER_COMPILED_OUT_ASSERT(path != nullptr);

    // Always start from safe defaults so missing keys are well-defined.
    impairment_config_default(cfg);

    FILE* fp = fopen(path, "r");
    if (fp == nullptr) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "Cannot open impairment config file: %s", path);
        return Result::ERR_IO;
    }

    Logger::log(Severity::INFO, "ConfigLoader", "Loading impairment config: %s", path);

    // Power of 10 Rule 2: bounded loop — at most MAX_CONFIG_LINES iterations.
    // Power of 10 Rule 3: fixed stack buffer for each line.
    char line[MAX_CONFIG_LINE_LEN];
    bool eof_reached = false;

    for (uint32_t i = 0U; i < MAX_CONFIG_LINES; ++i) {
        // CERT INT31-C: assert the uint32_t fits in int before narrowing cast.
        // MAX_CONFIG_LINE_LEN = 128, well within INT_MAX on all supported platforms.
        static_assert(MAX_CONFIG_LINE_LEN <= 2147483647U,
                      "MAX_CONFIG_LINE_LEN exceeds INT_MAX — fgets cast is unsafe");
        const char* got = fgets(line, static_cast<int>(MAX_CONFIG_LINE_LEN), fp);
        if (got == nullptr) {
            eof_reached = true;
            break;
        }
        parse_config_line(line, cfg);
    }

    if (!eof_reached) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "Config file has more than %u lines; remaining lines ignored",
                    MAX_CONFIG_LINES);
    }

    // Power of 10 Rule 7: check fclose() return value.
    int close_res = fclose(fp);
    if (close_res != 0) {
        Logger::log(Severity::WARNING_LO, "ConfigLoader",
                    "fclose() returned non-zero for: %s", path);
    }

    // Postconditions: probability values must be in [0.0, 1.0] after clamping.
    NEVER_COMPILED_OUT_ASSERT(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0);
    NEVER_COMPILED_OUT_ASSERT(cfg.duplication_probability >= 0.0 && cfg.duplication_probability <= 1.0);

    Logger::log(Severity::INFO, "ConfigLoader",
                "Impairment config loaded: enabled=%d latency=%u loss=%.3f seed=%llu",
                static_cast<int>(cfg.enabled),
                cfg.fixed_latency_ms,
                cfg.loss_probability,
                static_cast<unsigned long long>(cfg.prng_seed));

    return Result::OK;
}
