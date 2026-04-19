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
 * @file NcursesRenderer.hpp
 * @brief Three-panel ncurses display for StepDemo.
 *
 * Layout (left=client stats | centre=sequence diagram | right=server stats):
 *
 *   +----------------+--------------------------------+----------------+
 *   |  CLIENT        |  CLIENT  C.IMPR  S.IMPR  SERVER  |  SERVER        |
 *   |  sent:    N    |     |      |        |       |    |  recv:    N    |
 *   |  acked:   N    |     |--HELLO id=1----------->   |  drops:   N    |
 *   |  retries: N    |     |--DATA  id=2-->|            |  retries: N    |
 *   |  ...           |     |<--ACK  id=2---|            |  ...           |
 *   |                |     |--FAIL id=3--->X            |                |
 *   +----------------+--------------------------------+----------------+
 *   | [SPACE] step  [r] run/step  [i] impairment  [q] quit            |
 *   +------------------------------------------------------------------+
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, >=2 assertions per function, CC<=10.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: plain structs, explicit init/close lifecycle.
 *
 * Implements: REQ-7.2.5
 */
// Implements: REQ-7.2.5

#ifndef APP_NCURSES_RENDERER_HPP
#define APP_NCURSES_RENDERER_HPP

#include <ncurses.h>
// ncurses.h defines OK as a macro ((0)) which conflicts with Result::OK.
// Undefine it here so all downstream includes use the enum value correctly.
// MISRA advisory: macro suppression at a well-defined point; documented here.
#ifdef OK
#undef OK
#endif
#include <cstdint>
#include "core/DeliveryStats.hpp"
#include "app/StepController.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum number of wire events kept in the scrolling history.
static const uint32_t WIRE_HISTORY_MAX = 20U;

/// Minimum terminal width required to render the display.
static const int MIN_TERM_COLS = 60;

/// Minimum terminal height required to render the display.
static const int MIN_TERM_LINES = 10;

/// ncurses color pair IDs used by the renderer.
static const int COLOR_PAIR_GREEN  = 1;  ///< OK events (SEND_OK, ACK_RECEIVED)
static const int COLOR_PAIR_RED    = 2;  ///< Drop/fail events
static const int COLOR_PAIR_YELLOW = 3;  ///< Warning events (RETRY, TIMEOUT)
static const int COLOR_PAIR_CYAN   = 4;  ///< Control/HELLO events
static const int COLOR_PAIR_NORMAL = 5;  ///< Default text

// ─────────────────────────────────────────────────────────────────────────────
// WireDir — direction of a wire event for sequence diagram rendering
// ─────────────────────────────────────────────────────────────────────────────

/// Direction of a wire event used by the sequence diagram renderer.
enum WireDir : uint8_t {
    DIR_CLI_TO_SRV  = 0U,  ///< CLIENT → SERVER: full-span right-pointing arrow
    DIR_SRV_TO_CLI  = 1U,  ///< SERVER → CLIENT: full-span left-pointing arrow
    DIR_DROP        = 2U,  ///< Drop at unknown layer: label centred between all lifelines
    DIR_LOCAL       = 3U,  ///< Local event (timeout, cancel): label near CLIENT lifeline
    DIR_CLI_IMPAIR  = 4U,  ///< Outbound loss: CLIENT → ✗ at C.IMPR (never left client)
    DIR_SRV_IMPAIR  = 5U   ///< Inbound partition: arrow reaches S.IMPR then ✗ (dropped on arrival)
};

// ─────────────────────────────────────────────────────────────────────────────
// WireEntry — one event in the sequence diagram history
// ─────────────────────────────────────────────────────────────────────────────

/// One wire event entry stored in the circular history buffer.
struct WireEntry {
    char    text[80];    ///< Event label, null-terminated (no arrow prefix)
    int     color_pair;  ///< ncurses color pair for this entry
    WireDir dir;         ///< Arrow direction for sequence diagram rendering
    bool    active;      ///< True when this slot contains valid data
};

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer
// ─────────────────────────────────────────────────────────────────────────────

class NcursesRenderer {
public:
    /// Initialize ncurses, create windows, and define color pairs.
    /// Must be called once before any other method.
    void init();

    /// Shut down ncurses and restore the terminal.
    void close();

    /// Add one event to the sequence diagram history (overwrites oldest when full).
    /// @param[in] text        Null-terminated event label (no arrow prefix).
    /// @param[in] color_pair  ncurses color pair for this entry.
    /// @param[in] dir         Arrow direction for sequence diagram rendering.
    void add_wire_event(const char* text, int color_pair, WireDir dir);

    /// Add the HELLO startup event to the wire panel.
    void add_wire_hello(uint32_t client_node, uint32_t server_node);

    /// Clear all wire event history entries.
    void clear_wire();

    /// Redraw all panels with the current state.
    /// @param[in] cli_stats  Delivery stats snapshot from the client engine.
    /// @param[in] srv_stats  Delivery stats snapshot from the server engine.
    /// @param[in] ctrl       StepController for mode/profile/scenario display.
    void render(const DeliveryStats& cli_stats,
                const DeliveryStats& srv_stats,
                const StepController& ctrl);

    /// Non-blocking read of one ncurses key code.
    /// @return Key code, or ERR if no key is pending.
    int read_key();

    /// Blocking read of one ncurses key code (waits indefinitely).
    /// @return Key code.
    int read_key_blocking();

private:
    WINDOW*   m_win_client;  ///< Left panel — client stats
    WINDOW*   m_win_wire;    ///< Centre panel — wire events
    WINDOW*   m_win_server;  ///< Right panel — server stats
    WINDOW*   m_win_status;  ///< Bottom status bar

    WireEntry m_wire[WIRE_HISTORY_MAX];  ///< Circular wire event history
    uint32_t  m_wire_head;               ///< Next write index (mod WIRE_HISTORY_MAX)
    uint32_t  m_wire_count;              ///< Number of valid entries (<=WIRE_HISTORY_MAX)

    bool      m_initialized;  ///< True after init() succeeds

    /// Draw the client stats panel.
    void draw_client_panel(const DeliveryStats& stats);

    /// Draw the sequence diagram wire panel.
    void draw_wire_panel();

    /// Draw one sequence diagram row across four lifelines.
    /// @param[in] row          Row within m_win_wire to draw on (0-based).
    /// @param[in] cli_col      Column of the CLIENT lifeline.
    /// @param[in] cli_imp_col  Column of the C.IMPR lifeline.
    /// @param[in] srv_imp_col  Column of the S.IMPR lifeline.
    /// @param[in] srv_col      Column of the SERVER lifeline.
    /// @param[in] entry        Wire entry to render.
    void draw_seqdiag_row(int row, int cli_col, int cli_imp_col,
                          int srv_imp_col, int srv_col,
                          const WireEntry& entry) const;

    /// Draw the server stats panel.
    void draw_server_panel(const DeliveryStats& stats);

    /// Draw the status/keybinding bar.
    void draw_status_bar(const StepController& ctrl);

    /// Compute panel layout dimensions from current COLS/LINES.
    /// @param[out] left_w    Width of client panel (cols).
    /// @param[out] wire_w    Width of wire panel (cols).
    /// @param[out] right_w   Width of server panel (cols).
    /// @param[out] panel_h   Height of main panels (lines).
    void compute_layout(int& left_w, int& wire_w,
                        int& right_w, int& panel_h) const;
};

#endif // APP_NCURSES_RENDERER_HPP
