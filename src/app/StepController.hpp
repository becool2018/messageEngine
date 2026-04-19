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
 * @file StepController.hpp
 * @brief Step/run mode controller and scenario manager for StepDemo.
 *
 * Manages interactive STEP mode (SPACE to advance), RUN mode (auto-tick),
 * impairment profile selection, and scenario message progression.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, >=2 assertions per function, CC<=10.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: plain structs, explicit state.
 *
 * Implements: REQ-4.2.2
 */
// Implements: REQ-4.2.2

#ifndef APP_STEP_CONTROLLER_HPP
#define APP_STEP_CONTROLLER_HPP

#include <cstdint>
#include "core/Types.hpp"
#include "core/ImpairmentConfig.hpp"
#include "core/MessageEnvelope.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Enumerations
// ─────────────────────────────────────────────────────────────────────────────

/// Interactive mode: STEP waits for SPACE; RUN auto-advances at ~2 Hz.
enum StepMode : uint8_t {
    MODE_STEP = 0U,
    MODE_RUN  = 1U
};

/// Impairment profile to apply on the next session.
enum ImpairProfile : uint8_t {
    PROF_CLEAN     = 0U,   ///< No impairment
    PROF_LOSSY     = 1U,   ///< 20% loss probability
    PROF_REORDER   = 2U,   ///< Reorder with jitter
    PROF_PARTITION = 3U,   ///< 2s partition then recover
    PROF_COUNT     = 4U
};

// ─────────────────────────────────────────────────────────────────────────────
// ScenarioEntry — one message in the fixed demo script
// ─────────────────────────────────────────────────────────────────────────────

/// One entry in the fixed scenario script.
struct ScenarioEntry {
    ReliabilityClass reliability;   ///< Message reliability class
    const char*      label;         ///< Short human-readable label
};

/// Number of messages in the scenario script.
static const uint32_t SCENARIO_COUNT = 4U;

// ─────────────────────────────────────────────────────────────────────────────
// StepController
// ─────────────────────────────────────────────────────────────────────────────

class StepController {
public:
    /// Zero-initialize all state. Must be called once before use.
    void init();

    /// Handle a raw ncurses key code.
    /// SPACE: advance in STEP mode (no-op in RUN).
    /// 'r':   toggle STEP/RUN mode.
    /// 'i':   cycle impairment profile and request session restart.
    /// 'n':   restart session immediately (keep current profile).
    /// 'q':   request quit.
    void handle_key(int ch);

    /// True when the user pressed 'q'.
    bool is_quit() const;

    /// True when 'i' was pressed — caller must reinitialize session.
    bool is_restart_requested() const;

    /// Clear the restart flag after the session has restarted.
    void clear_restart();

    /// True in STEP mode when SPACE was pressed this cycle.
    /// Cleared after do_tick() returns.
    bool consume_step();

    /// Current step/run mode.
    StepMode get_mode() const;

    /// Current impairment profile (for the next / current session).
    ImpairProfile get_profile() const;

    /// Human-readable name for the current impairment profile.
    const char* profile_name() const;

    /// Index of the scenario message about to be sent (0..SCENARIO_COUNT).
    uint32_t scenario_idx() const;

    /// True when all scenario messages have been sent.
    bool scenario_done() const;

    /// Reference to the current scenario entry.
    const ScenarioEntry& current_scenario() const;

    /// Advance to the next scenario message.
    void advance_scenario();

    /// Reset scenario to the beginning (called on session restart).
    void reset_scenario();

    /// Fill an ImpairmentConfig for the given profile.
    static void fill_impairment(ImpairmentConfig& cfg, ImpairProfile profile);

private:
    StepMode      m_mode;
    ImpairProfile m_profile;
    uint32_t      m_scenario_idx;
    bool          m_quit;
    bool          m_restart;
    bool          m_step_pending;   ///< SPACE pressed; tick not yet consumed
};

#endif // APP_STEP_CONTROLLER_HPP
