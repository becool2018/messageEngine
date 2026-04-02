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
// Test 13: duplication_probability negative → clamped to 0.0 (L73 True)
// ─────────────────────────────────────────────────────────────────────────────
static void test_neg_dup_probability()
{
    write_test_file("duplication_probability=-0.5\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.duplication_probability == 0.0);  // L73 True: clamped from -0.5
    assert(cfg.duplication_probability >= 0.0);

    printf("PASS: test_neg_dup_probability\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: leading spaces on line — stripped before parsing (L134 ' ' True)
//          Also includes an all-whitespace line at EOF (L137 '\0' True)
// ─────────────────────────────────────────────────────────────────────────────
static void test_leading_space()
{
    // Third "line" is "  " (spaces, no trailing newline) → '\0' True after stripping
    write_test_file("  enabled=1\n  fixed_latency_ms=50\n  ");

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);           // L134 ' ' True: leading spaces stripped
    assert(cfg.fixed_latency_ms == 50U);

    printf("PASS: test_leading_space\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: leading tab on line — stripped before parsing (L134 '\t' True)
// ─────────────────────────────────────────────────────────────────────────────
static void test_leading_tab()
{
    write_test_file("\tenabled=1\n\tfixed_latency_ms=75\n");

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    assert(cfg.fixed_latency_ms == 75U);

    printf("PASS: test_leading_tab\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: CRLF line ending — '\r' treated as skip (L137 '\r' True)
// ─────────────────────────────────────────────────────────────────────────────
static void test_carriage_return()
{
    // The blank line is "\r\n": fgets returns "\r\n"; after stripping no spaces,
    // *p == '\r' → L137 '\r' True → line is skipped.
    write_test_file("enabled=1\n\r\nfixed_latency_ms=10\n");

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == true);
    assert(cfg.fixed_latency_ms == 10U);

    printf("PASS: test_carriage_return\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: truly malformed line — sscanf parses only 1 token (L154 True)
//   "keyonly" has no separator (space, tab, or '='), so sscanf returns 1 not 2.
// ─────────────────────────────────────────────────────────────────────────────
static void test_truly_malformed()
{
    write_test_file("keyonly\nfixed_latency_ms=30\n");

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    // "keyonly" line skipped (L154 True: n != 2); next line parses correctly
    assert(cfg.fixed_latency_ms == 30U);
    assert(cfg.enabled == false);

    printf("PASS: test_truly_malformed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: file with > MAX_CONFIG_LINES lines — loop exits at bound (L192 False)
//          and a warning is logged for remaining lines (L201 True)
// ─────────────────────────────────────────────────────────────────────────────
static void test_over_64_lines()
{
    // Generate 65 lines (one more than MAX_CONFIG_LINES = 64)
    // Power of 10: stack buffer sized for 65 × 11 bytes + null
    char content[65 * 11 + 2];
    int pos = 0;
    // Power of 10: fixed loop bound
    for (int i = 0; i < 65; ++i) {
        int n = snprintf(content + pos,
                         static_cast<size_t>(static_cast<int>(sizeof(content)) - pos),
                         "enabled=0\n");
        assert(n > 0 && n < static_cast<int>(sizeof(content)) - pos);
        pos += n;
    }

    write_test_file(content);

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    // L192 False: loop hit MAX_CONFIG_LINES limit before EOF
    // L201 True:  !eof_reached → warning logged
    assert(res == Result::OK);
    assert(cfg.enabled == false);   // all 64 parsed lines say enabled=0

    printf("PASS: test_over_64_lines\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: malformed values for every known key — sscanf returns 0, defaults retained
//   Covers the False branch of every `if (sscanf(val, ...) == 1)` in apply_kv().
//   Passing a non-numeric token (e.g. "abc") makes sscanf return 0, so the
//   assignment inside the if is skipped and impairment_config_default() values
//   remain in effect.
// Verifies: REQ-5.2.1
// Verification: M1 + M2 + M4 + M5 (fault injection not required — file I/O tested via tmp files)
// ─────────────────────────────────────────────────────────────────────────────
static void test_malformed_values()
{
    // Every known key is present but has a non-numeric value ("abc") so that
    // sscanf fails for each one — exercising the False branch of every
    // `if (sscanf == 1)` guard inside apply_kv().
    write_test_file(
        "enabled=abc\n"
        "fixed_latency_ms=abc\n"
        "jitter_mean_ms=abc\n"
        "jitter_variance_ms=abc\n"
        "loss_probability=abc\n"
        "duplication_probability=abc\n"
        "reorder_enabled=abc\n"
        "reorder_window_size=abc\n"
        "partition_enabled=abc\n"
        "partition_duration_ms=abc\n"
        "partition_gap_ms=abc\n"
        "prng_seed=abc\n"
    );

    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    // All values must remain at impairment_config_default() values
    assert(cfg.enabled == false);
    assert(cfg.fixed_latency_ms == 0U);
    assert(cfg.jitter_mean_ms == 0U);
    assert(cfg.jitter_variance_ms == 0U);
    assert(cfg.loss_probability == 0.0);
    assert(cfg.duplication_probability == 0.0);
    assert(cfg.reorder_enabled == false);
    assert(cfg.reorder_window_size == 0U);
    assert(cfg.partition_enabled == false);
    assert(cfg.partition_duration_ms == 0U);
    assert(cfg.partition_gap_ms == 0U);
    assert(cfg.prng_seed == 42ULL);  // default seed unchanged

    printf("PASS: test_malformed_values\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: parse_uint trailing garbage — *end != '\0' True branch
//   "50abc": strtoul consumes "50", end points to "abc", *end != '\0' → reject.
//   Result: field unchanged at default.
// Verifies: REQ-5.2.1
// ─────────────────────────────────────────────────────────────────────────────
static void test_parse_uint_trailing_garbage()
{
    // Verifies: REQ-5.2.1
    write_test_file("fixed_latency_ms=50abc\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.fixed_latency_ms == 0U);  // rejected — default retained
    assert(cfg.enabled == false);

    printf("PASS: test_parse_uint_trailing_garbage\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: parse_bool trailing garbage — *end != '\0' True branch
//   "1z": strtoul consumes "1", end points to "z", *end != '\0' → reject.
// Verifies: REQ-5.2.1
// ─────────────────────────────────────────────────────────────────────────────
static void test_parse_bool_trailing_garbage()
{
    // Verifies: REQ-5.2.1
    write_test_file("enabled=1z\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.enabled == false);  // rejected — default retained
    assert(cfg.fixed_latency_ms == 0U);

    printf("PASS: test_parse_bool_trailing_garbage\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: parse_prob trailing garbage — *end != '\0' True branch
//   "0.5x": strtod consumes "0.5", end points to "x", *end != '\0' → reject.
// Verifies: REQ-5.2.1
// ─────────────────────────────────────────────────────────────────────────────
static void test_parse_prob_trailing_garbage()
{
    // Verifies: REQ-5.2.1
    write_test_file("loss_probability=0.5x\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.loss_probability == 0.0);  // rejected — default retained
    assert(cfg.duplication_probability == 0.0);

    printf("PASS: test_parse_prob_trailing_garbage\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 23: parse_u64 trailing garbage — *end != '\0' True branch
//   "999q": strtoull consumes "999", end points to "q", *end != '\0' → reject.
// Verifies: REQ-5.2.1
// ─────────────────────────────────────────────────────────────────────────────
static void test_parse_u64_trailing_garbage()
{
    // Verifies: REQ-5.2.1
    write_test_file("prng_seed=999q\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.prng_seed == 42ULL);  // rejected — default seed retained
    assert(cfg.enabled == false);

    printf("PASS: test_parse_u64_trailing_garbage\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 24: apply_reorder_window trailing garbage — *end != '\0' True branch
//   "4k": strtoul consumes "4", end points to "k", *end != '\0' → reject.
// Verifies: REQ-5.2.1
// ─────────────────────────────────────────────────────────────────────────────
static void test_reorder_window_trailing_garbage()
{
    // Verifies: REQ-5.2.1
    write_test_file("reorder_window_size=4k\n");
    ImpairmentConfig cfg;
    Result res = impairment_config_load(TEST_FILE, cfg);

    assert(res == Result::OK);
    assert(cfg.reorder_window_size == 0U);  // rejected — default retained
    assert(cfg.reorder_enabled == false);

    printf("PASS: test_reorder_window_trailing_garbage\n");
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
    test_neg_dup_probability();
    test_leading_space();
    test_leading_tab();
    test_carriage_return();
    test_truly_malformed();
    test_over_64_lines();
    test_malformed_values();
    test_parse_uint_trailing_garbage();
    test_parse_bool_trailing_garbage();
    test_parse_prob_trailing_garbage();
    test_parse_u64_trailing_garbage();
    test_reorder_window_trailing_garbage();

    // Cleanup temp file
    (void)remove(TEST_FILE);

    printf("ALL ImpairmentConfigLoader tests passed.\n");
    return 0;
}
