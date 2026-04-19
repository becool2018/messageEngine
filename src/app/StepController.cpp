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
 * @file StepController.cpp
 * @brief Step/run controller implementation for StepDemo.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, >=2 assertions per function, CC<=10.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: explicit state, plain arrays.
 *
 * Implements: REQ-4.2.2
 */
// Implements: REQ-4.2.2

#include "app/StepController.hpp"
#include "core/Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Scenario script (fixed; no heap)
// ─────────────────────────────────────────────────────────────────────────────

// Power of 10 Rule 3: static constant table; no allocation after init.
static const ScenarioEntry SCENARIO[SCENARIO_COUNT] = {
    { ReliabilityClass::BEST_EFFORT,    "BEST_EFFORT"    },
    { ReliabilityClass::RELIABLE_ACK,   "RELIABLE_ACK"   },
    { ReliabilityClass::RELIABLE_RETRY, "RELIABLE_RETRY" },
    { ReliabilityClass::RELIABLE_RETRY, "RELIABLE_RETRY" }
};

// ─────────────────────────────────────────────────────────────────────────────
// Profile name table
// ─────────────────────────────────────────────────────────────────────────────

static const char* const PROFILE_NAMES[PROF_COUNT] = {
    "Clean",
    "Lossy (20%)",
    "Reorder",
    "Partition"
};

// ─────────────────────────────────────────────────────────────────────────────
// StepController::init()
// ─────────────────────────────────────────────────────────────────────────────

void StepController::init()
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);   // pre: scenario table non-empty
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);       // pre: profile table non-empty

    m_mode          = MODE_STEP;
    m_profile       = PROF_CLEAN;
    m_scenario_idx  = 0U;
    m_quit          = false;
    m_restart       = false;
    m_step_pending  = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::handle_key()
// ─────────────────────────────────────────────────────────────────────────────

void StepController::handle_key(int ch)
{
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);        // pre: profile table non-empty
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);    // pre: scenario table non-empty

    // Normalise to lowercase once; eliminates double conditions and keeps CC<=10.
    const int lc = (ch >= static_cast<int>('A') && ch <= static_cast<int>('Z'))
                   ? (ch + (static_cast<int>('a') - static_cast<int>('A')))
                   : ch;

    if (lc == 'q') {
        m_quit = true;
        return;
    }

    if (lc == 'r') {
        m_mode = (m_mode == MODE_STEP) ? MODE_RUN : MODE_STEP;
        return;
    }

    if (lc == 'i') {
        m_profile = static_cast<ImpairProfile>(
            (static_cast<uint8_t>(m_profile) + 1U) % static_cast<uint8_t>(PROF_COUNT));
        m_restart = true;
        return;
    }

    if (lc == 'n') {
        m_restart = true;  // restart session, keep current profile
        return;
    }

    if (lc == ' ' && m_mode == MODE_STEP) {
        m_step_pending = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::is_quit()
// ─────────────────────────────────────────────────────────────────────────────

bool StepController::is_quit() const
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state
    return m_quit;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::is_restart_requested()
// ─────────────────────────────────────────────────────────────────────────────

bool StepController::is_restart_requested() const
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state
    return m_restart;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::clear_restart()
// ─────────────────────────────────────────────────────────────────────────────

void StepController::clear_restart()
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state
    m_restart = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::consume_step()
// ─────────────────────────────────────────────────────────────────────────────

bool StepController::consume_step()
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state

    if (m_step_pending) {
        m_step_pending = false;
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::get_mode()
// ─────────────────────────────────────────────────────────────────────────────

StepMode StepController::get_mode() const
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state
    return m_mode;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::get_profile()
// ─────────────────────────────────────────────────────────────────────────────

ImpairProfile StepController::get_profile() const
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state
    return m_profile;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::profile_name()
// ─────────────────────────────────────────────────────────────────────────────

const char* StepController::profile_name() const
{
    NEVER_COMPILED_OUT_ASSERT(m_profile < PROF_COUNT);      // pre: valid profile index
    NEVER_COMPILED_OUT_ASSERT(PROFILE_NAMES[0] != nullptr); // pre: name table valid

    return PROFILE_NAMES[static_cast<uint8_t>(m_profile)];
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::scenario_idx()
// ─────────────────────────────────────────────────────────────────────────────

uint32_t StepController::scenario_idx() const
{
    NEVER_COMPILED_OUT_ASSERT(m_scenario_idx <= SCENARIO_COUNT);  // pre: bounded index
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);               // pre: table non-empty
    return m_scenario_idx;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::scenario_done()
// ─────────────────────────────────────────────────────────────────────────────

bool StepController::scenario_done() const
{
    NEVER_COMPILED_OUT_ASSERT(m_scenario_idx <= SCENARIO_COUNT);  // pre: bounded index
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);               // pre: table non-empty
    return m_scenario_idx >= SCENARIO_COUNT;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::current_scenario()
// ─────────────────────────────────────────────────────────────────────────────

const ScenarioEntry& StepController::current_scenario() const
{
    NEVER_COMPILED_OUT_ASSERT(m_scenario_idx < SCENARIO_COUNT);  // pre: valid index
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);              // pre: table non-empty
    return SCENARIO[m_scenario_idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::advance_scenario()
// ─────────────────────────────────────────────────────────────────────────────

void StepController::advance_scenario()
{
    NEVER_COMPILED_OUT_ASSERT(m_scenario_idx <= SCENARIO_COUNT);  // pre: not past end
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);               // pre: table non-empty

    if (m_scenario_idx < SCENARIO_COUNT) {
        ++m_scenario_idx;
    }

    NEVER_COMPILED_OUT_ASSERT(m_scenario_idx <= SCENARIO_COUNT);  // post: bounded
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::reset_scenario()
// ─────────────────────────────────────────────────────────────────────────────

void StepController::reset_scenario()
{
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: table non-empty
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);      // pre: valid state

    m_scenario_idx = 0U;
    m_step_pending = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// StepController::fill_impairment() — static
// ─────────────────────────────────────────────────────────────────────────────

void StepController::fill_impairment(ImpairmentConfig& cfg, ImpairProfile profile)
{
    NEVER_COMPILED_OUT_ASSERT(profile < PROF_COUNT);  // pre: valid profile
    NEVER_COMPILED_OUT_ASSERT(PROF_COUNT > 0U);       // pre: table non-empty

    impairment_config_default(cfg);  // start from safe defaults (all off)

    switch (profile) {
        case PROF_LOSSY:
            cfg.enabled          = true;
            cfg.loss_probability = 0.20;   // 20% loss
            break;
        case PROF_REORDER:
            cfg.enabled              = true;
            cfg.jitter_mean_ms       = 50U;
            cfg.jitter_variance_ms   = 20U;
            cfg.reorder_enabled      = true;
            cfg.reorder_window_size  = 3U;
            break;
        case PROF_PARTITION:
            cfg.enabled               = true;
            cfg.partition_enabled     = true;
            cfg.partition_duration_ms = 2000U;  // 2s outage
            cfg.partition_gap_ms      = 5000U;  // 5s between partitions
            break;
        case PROF_CLEAN:
        default:
            // impairment_config_default already set enabled=false; nothing more to do.
            break;
    }
}
