/**
 * @file test_ImpairmentConfigLoader.cpp
 * @brief Unit tests for impairment_config_load(): file parsing, defaults, clamping.
 *
 * Tests cover all branches of ImpairmentConfigLoader.cpp targeting ≥75%
 * branch coverage.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-5.2.1
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "platform/ImpairmentConfigLoader.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture helpers — write temporary INI files to /tmp
// ─────────────────────────────────────────────────────────────────────────────

static const char* TEST_FILE = "/tmp/test_icl_config.ini";

static void write_test_file(const char* content)
{
    FILE* fp = fopen(TEST_FILE, "w");
    assert(fp != nullptr);
    int r = fputs(content, fp);
    assert(r >= 0);
    int c = fclose(fp);
    assert(c == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: file not found — ERR_IO returned; cfg populated with defaults
// ─────────────────────────────────────────────────────────────────────────────
static void test_file_not_found()
{
    ImpairmentConfig cfg;
    (void)memset(&cfg, 0xFF, sizeof(cfg));   // poison to detect default not applied

    Result res = impairment_config_load("/tmp/this_file_does_not_exist_icl.ini", cfg);

    assert(res == Result::ERR_IO);
    // impairment_config_default() must have been called even on ERR_IO
    assert(cfg.enabled == false);
    assert(cfg.fixed_latency_ms == 0U);

    printf("PASS: test_file_not_found\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: empty file — OK returned; all fields retain defaults
// ─────────────────────────────────────────────────────────────────────────────
static void test_empty_file()
{
    write_test_file("");

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == false);
    assert(cfg.loss_probability == 0.0);
    assert(cfg.prng_seed == 42ULL);

    printf("PASS: test_empty_file\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: all fields present — all values loaded correctly
// ─────────────────────────────────────────────────────────────────────────────
static void test_all_fields()
{
    write_test_file(
        "enabled=1\n"
        "fixed_latency_ms=50\n"
        "jitter_mean_ms=10\n"
        "jitter_variance_ms=5\n"
        "loss_probability=0.10\n"
        "duplication_probability=0.05\n"
        "reorder_enabled=1\n"
        "reorder_window_size=8\n"
        "partition_enabled=1\n"
        "partition_duration_ms=1000\n"
        "partition_gap_ms=2000\n"
        "prng_seed=12345\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    assert(cfg.fixed_latency_ms == 50U);
    assert(cfg.jitter_mean_ms == 10U);
    assert(cfg.jitter_variance_ms == 5U);
    assert(cfg.loss_probability > 0.09 && cfg.loss_probability < 0.11);
    assert(cfg.duplication_probability > 0.04 && cfg.duplication_probability < 0.06);
    assert(cfg.reorder_enabled == true);
    assert(cfg.reorder_window_size == 8U);
    assert(cfg.partition_enabled == true);
    assert(cfg.partition_duration_ms == 1000U);
    assert(cfg.partition_gap_ms == 2000U);
    assert(cfg.prng_seed == 12345ULL);

    printf("PASS: test_all_fields\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: partial file — missing keys retain default values
// ─────────────────────────────────────────────────────────────────────────────
static void test_partial_file()
{
    write_test_file(
        "enabled=1\n"
        "fixed_latency_ms=100\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    assert(cfg.fixed_latency_ms == 100U);
    // Missing keys must hold defaults
    assert(cfg.loss_probability == 0.0);
    assert(cfg.partition_enabled == false);
    assert(cfg.prng_seed == 42ULL);

    printf("PASS: test_partial_file\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: comment and blank lines — ignored; values loaded correctly
// ─────────────────────────────────────────────────────────────────────────────
static void test_comment_and_blank_lines()
{
    write_test_file(
        "# This is a comment\n"
        "\n"
        "; Another comment style\n"
        "enabled=1\n"
        "\n"
        "# prng_seed left at default\n"
        "fixed_latency_ms=25\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    assert(cfg.fixed_latency_ms == 25U);
    // Comment lines must not corrupt adjacent fields
    assert(cfg.prng_seed == 42ULL);

    printf("PASS: test_comment_and_blank_lines\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: whitespace around '=' — supported syntax
// ─────────────────────────────────────────────────────────────────────────────
static void test_whitespace_around_equals()
{
    write_test_file(
        "enabled = 1\n"
        "fixed_latency_ms  =  200\n"
        "prng_seed = 99\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    assert(cfg.fixed_latency_ms == 200U);
    assert(cfg.prng_seed == 99ULL);

    printf("PASS: test_whitespace_around_equals\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: malformed line (no '=') — skipped; other values load correctly
// ─────────────────────────────────────────────────────────────────────────────
static void test_malformed_line()
{
    write_test_file(
        "this line has no equals sign\n"
        "fixed_latency_ms=30\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    // Malformed line must not corrupt any field
    assert(cfg.fixed_latency_ms == 30U);
    assert(cfg.enabled == false);

    printf("PASS: test_malformed_line\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: unknown key — logged and ignored; other values unaffected
// ─────────────────────────────────────────────────────────────────────────────
static void test_unknown_key()
{
    write_test_file(
        "unknown_field=42\n"
        "enabled=1\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    // Unknown key must not corrupt any other field
    assert(cfg.fixed_latency_ms == 0U);

    printf("PASS: test_unknown_key\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: loss_probability clamped to [0.0, 1.0]
// ─────────────────────────────────────────────────────────────────────────────
static void test_loss_probability_clamp()
{
    // Test clamping above 1.0
    write_test_file("loss_probability=2.5\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.loss_probability == 1.0);

    // Test clamping below 0.0 (negative value)
    write_test_file("loss_probability=-0.5\n");
    ImpairmentConfig cfg2;
    Result res2 = impairment_config_load(TEST_FILE, cfg2);

    assert(res2 == Result::OK);
    assert(cfg2.loss_probability == 0.0);

    printf("PASS: test_loss_probability_clamp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: duplication_probability clamped to [0.0, 1.0]
// ─────────────────────────────────────────────────────────────────────────────
static void test_duplication_probability_clamp()
{
    write_test_file("duplication_probability=5.0\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.duplication_probability == 1.0);
    assert(res != Result::ERR_IO);

    printf("PASS: test_duplication_probability_clamp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: reorder_window_size clamped to IMPAIR_DELAY_BUF_SIZE
// ─────────────────────────────────────────────────────────────────────────────
static void test_reorder_window_size_clamp()
{
    write_test_file("reorder_window_size=9999\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.reorder_window_size == IMPAIR_DELAY_BUF_SIZE);
    assert(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE);

    printf("PASS: test_reorder_window_size_clamp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: large prng_seed — uint64_t value loaded without truncation
// ─────────────────────────────────────────────────────────────────────────────
static void test_large_prng_seed()
{
    write_test_file("prng_seed=18446744073709551615\n");  // UINT64_MAX
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.prng_seed == 18446744073709551615ULL);
    assert(cfg.prng_seed != 42ULL);

    printf("PASS: test_large_prng_seed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    test_file_not_found();
    test_empty_file();
    test_all_fields();
    test_partial_file();
    test_comment_and_blank_lines();
    test_whitespace_around_equals();
    test_malformed_line();
    test_unknown_key();
    test_loss_probability_clamp();
    test_duplication_probability_clamp();
    test_reorder_window_size_clamp();
    test_large_prng_seed();

    // Cleanup temp file
    (void)remove(TEST_FILE);

    printf("ALL ImpairmentConfigLoader tests passed.\n");
    return 0;
}
