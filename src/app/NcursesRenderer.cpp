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
 * @file NcursesRenderer.cpp
 * @brief Three-panel ncurses display for StepDemo.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, >=2 assertions per function, CC<=10.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: explicit init/close, bounded loops.
 *
 * MISRA advisory: ncurses return values are cast to (void) where failure
 * is non-fatal and cannot be meaningfully acted upon (display glitches only).
 *
 * Implements: REQ-7.2.5
 */
// Implements: REQ-7.2.5

#include "app/NcursesRenderer.hpp"
#include "core/Assert.hpp"
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::init()
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::init()
{
    NEVER_COMPILED_OUT_ASSERT(WIRE_HISTORY_MAX > 0U);   // pre: capacity valid
    NEVER_COMPILED_OUT_ASSERT(WIRE_HISTORY_MAX <= 256U); // pre: reasonable bound

    (void)initscr();
    (void)cbreak();
    (void)noecho();
    (void)keypad(stdscr, TRUE);
    (void)nodelay(stdscr, TRUE);  // non-blocking getch by default
    (void)curs_set(0);            // hide cursor

    if (has_colors() == TRUE) {
        (void)start_color();
        (void)init_pair(static_cast<short>(COLOR_PAIR_GREEN),
                        COLOR_GREEN,  COLOR_BLACK);
        (void)init_pair(static_cast<short>(COLOR_PAIR_RED),
                        COLOR_RED,    COLOR_BLACK);
        (void)init_pair(static_cast<short>(COLOR_PAIR_YELLOW),
                        COLOR_YELLOW, COLOR_BLACK);
        (void)init_pair(static_cast<short>(COLOR_PAIR_CYAN),
                        COLOR_CYAN,   COLOR_BLACK);
        (void)init_pair(static_cast<short>(COLOR_PAIR_NORMAL),
                        COLOR_WHITE,  COLOR_BLACK);
    }

    m_wire_head  = 0U;
    m_wire_count = 0U;
    (void)memset(m_wire, 0, sizeof(m_wire));

    // Compute initial layout and create sub-windows.
    int left_w  = 0;
    int wire_w  = 0;
    int right_w = 0;
    int panel_h = 0;
    compute_layout(left_w, wire_w, right_w, panel_h);

    // Row 0 = title bar (drawn directly on stdscr).
    // Rows 1..panel_h = three panels side by side.
    // Row panel_h+1 = status bar.
    m_win_client = newwin(panel_h, left_w,  1, 0);
    m_win_wire   = newwin(panel_h, wire_w,  1, left_w);
    m_win_server = newwin(panel_h, right_w, 1, left_w + wire_w);
    m_win_status = newwin(1, COLS, LINES - 1, 0);

    m_initialized = true;

    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // post: renderer ready
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::close()
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::close()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);           // pre: was initialized
    NEVER_COMPILED_OUT_ASSERT(WIRE_HISTORY_MAX > 0U);   // pre: capacity valid

    if (m_win_client != nullptr) { (void)delwin(m_win_client); m_win_client = nullptr; }
    if (m_win_wire   != nullptr) { (void)delwin(m_win_wire);   m_win_wire   = nullptr; }
    if (m_win_server != nullptr) { (void)delwin(m_win_server); m_win_server = nullptr; }
    if (m_win_status != nullptr) { (void)delwin(m_win_status); m_win_status = nullptr; }

    (void)endwin();
    m_initialized = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::add_wire_event()
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::add_wire_event(const char* text, int color_pair, WireDir dir)
{
    NEVER_COMPILED_OUT_ASSERT(text != nullptr);                 // pre: text not null
    NEVER_COMPILED_OUT_ASSERT(m_wire_head < WIRE_HISTORY_MAX);  // pre: head in bounds

    WireEntry& slot = m_wire[m_wire_head];
    slot.active     = true;
    slot.color_pair = color_pair;
    slot.dir        = dir;

    // Bounded copy: sizeof(slot.text) - 1 chars max, always null-terminated.
    (void)strncpy(slot.text, text, sizeof(slot.text) - 1U);
    slot.text[sizeof(slot.text) - 1U] = '\0';

    m_wire_head = (m_wire_head + 1U) % WIRE_HISTORY_MAX;
    if (m_wire_count < WIRE_HISTORY_MAX) {
        ++m_wire_count;
    }

    NEVER_COMPILED_OUT_ASSERT(m_wire_count <= WIRE_HISTORY_MAX);  // post: bounded
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::add_wire_hello()
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::add_wire_hello(uint32_t client_node, uint32_t server_node)
{
    NEVER_COMPILED_OUT_ASSERT(client_node != 0U);  // pre: valid client node
    NEVER_COMPILED_OUT_ASSERT(server_node != 0U);  // pre: valid server node

    char buf[80] = {};
    (void)snprintf(buf, sizeof(buf), "HELLO [src=%u dst=%u]",
                   client_node, server_node);
    add_wire_event(buf, COLOR_PAIR_CYAN, DIR_CLI_TO_SRV);
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::clear_wire()
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::clear_wire()
{
    NEVER_COMPILED_OUT_ASSERT(WIRE_HISTORY_MAX > 0U);   // pre: capacity valid
    NEVER_COMPILED_OUT_ASSERT(m_wire_count <= WIRE_HISTORY_MAX); // pre: bounded

    (void)memset(m_wire, 0, sizeof(m_wire));
    m_wire_head  = 0U;
    m_wire_count = 0U;

    NEVER_COMPILED_OUT_ASSERT(m_wire_count == 0U);  // post: cleared
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::render()
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::render(const DeliveryStats& cli_stats,
                              const DeliveryStats& srv_stats,
                              const StepController& ctrl)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // pre: renderer ready
    NEVER_COMPILED_OUT_ASSERT(m_win_client != nullptr);  // pre: windows created

    // Draw title bar on stdscr
    (void)move(0, 0);
    (void)clrtoeol();
    (void)attron(A_BOLD);
    (void)mvprintw(0, 1, "StepDemo — Message Engine Communication Visualizer");
    (void)attroff(A_BOLD);

    (void)werase(m_win_client);
    (void)werase(m_win_wire);
    (void)werase(m_win_server);
    (void)werase(m_win_status);

    draw_client_panel(cli_stats);
    draw_wire_panel();
    draw_server_panel(srv_stats);
    draw_status_bar(ctrl);

    // Batch refresh for flicker-free update.
    (void)wnoutrefresh(stdscr);
    (void)wnoutrefresh(m_win_client);
    (void)wnoutrefresh(m_win_wire);
    (void)wnoutrefresh(m_win_server);
    (void)wnoutrefresh(m_win_status);
    (void)doupdate();
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::read_key()
// ─────────────────────────────────────────────────────────────────────────────

int NcursesRenderer::read_key()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // pre: renderer ready
    NEVER_COMPILED_OUT_ASSERT(m_win_client != nullptr);  // pre: windows exist
    return getch();  // non-blocking (nodelay was set in init())
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::read_key_blocking()
// ─────────────────────────────────────────────────────────────────────────────

int NcursesRenderer::read_key_blocking()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // pre: renderer ready
    NEVER_COMPILED_OUT_ASSERT(m_win_client != nullptr);  // pre: windows exist

    (void)nodelay(stdscr, FALSE);   // switch to blocking
    int ch = getch();
    (void)nodelay(stdscr, TRUE);    // restore non-blocking
    return ch;
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::compute_layout() — private
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::compute_layout(int& left_w, int& wire_w,
                                      int& right_w, int& panel_h) const
{
    NEVER_COMPILED_OUT_ASSERT(COLS  >= MIN_TERM_COLS);   // pre: terminal wide enough
    NEVER_COMPILED_OUT_ASSERT(LINES >= MIN_TERM_LINES);  // pre: terminal tall enough

    left_w  = 22;
    right_w = 22;
    wire_w  = COLS - left_w - right_w;
    if (wire_w < 16) {
        wire_w  = COLS / 2;
        left_w  = (COLS - wire_w) / 2;
        right_w = COLS - wire_w - left_w;
    }
    panel_h = LINES - 2;  // -1 title row, -1 status row
    if (panel_h < 4) {
        panel_h = 4;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::draw_client_panel() — private
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::draw_client_panel(const DeliveryStats& stats)
{
    NEVER_COMPILED_OUT_ASSERT(m_win_client != nullptr);  // pre: window valid
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // pre: renderer ready

    // Row 1: title (row 0 is the top border drawn by wborder() at the end).
    (void)wattron(m_win_client, A_BOLD);
    (void)mvwprintw(m_win_client, 1, 1, "CLIENT");
    (void)wattroff(m_win_client, A_BOLD);
    (void)mvwhline(m_win_client, 2, 0, ACS_HLINE, getmaxx(m_win_client));

    (void)mvwprintw(m_win_client, 3, 1, "sent:    %4u", stats.msgs_sent);
    (void)mvwprintw(m_win_client, 4, 1, "recv:    %4u", stats.msgs_received);
    (void)mvwprintw(m_win_client, 5, 1, "acked:   %4u", stats.ack.acks_received);
    (void)mvwprintw(m_win_client, 6, 1, "retries: %4u", stats.retry.retries_sent);
    (void)mvwprintw(m_win_client, 7, 1, "timeouts:%4u", stats.ack.timeouts);
    (void)mvwprintw(m_win_client, 8, 1, "expired: %4u", stats.msgs_dropped_expired);

    (void)mvwhline(m_win_client, 10, 0, ACS_HLINE, getmaxx(m_win_client));
    (void)wattron(m_win_client, A_BOLD);
    (void)mvwprintw(m_win_client, 11, 1, "IMPAIRMENT");
    (void)wattroff(m_win_client, A_BOLD);
    (void)mvwprintw(m_win_client, 12, 1, "loss:    %4u", stats.impairment.loss_drops);
    (void)mvwprintw(m_win_client, 13, 1, "reorder: %4u", stats.impairment.reorder_buffered);
    (void)mvwprintw(m_win_client, 14, 1, "dups:    %4u", stats.impairment.duplicate_injects);
    (void)mvwprintw(m_win_client, 15, 1, "part:    %4u", stats.impairment.partition_drops);

    (void)wborder(m_win_client, ACS_VLINE, ACS_VLINE,
                  ACS_HLINE, ACS_HLINE,
                  ACS_ULCORNER, ACS_URCORNER,
                  ACS_LLCORNER, ACS_LRCORNER);
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::draw_server_panel() — private
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::draw_server_panel(const DeliveryStats& stats)
{
    NEVER_COMPILED_OUT_ASSERT(m_win_server != nullptr);  // pre: window valid
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // pre: renderer ready

    // Row 1: title (row 0 is the top border drawn by wborder() at the end).
    (void)wattron(m_win_server, A_BOLD);
    (void)mvwprintw(m_win_server, 1, 1, "SERVER");
    (void)wattroff(m_win_server, A_BOLD);
    (void)mvwhline(m_win_server, 2, 0, ACS_HLINE, getmaxx(m_win_server));

    (void)mvwprintw(m_win_server, 3, 1, "recv:    %4u", stats.msgs_received);
    (void)mvwprintw(m_win_server, 4, 1, "sent:    %4u", stats.msgs_sent);
    (void)mvwprintw(m_win_server, 5, 1, "acked:   %4u", stats.ack.acks_received);
    (void)mvwprintw(m_win_server, 6, 1, "retries: %4u", stats.retry.retries_sent);
    (void)mvwprintw(m_win_server, 7, 1, "dups:    %4u", stats.msgs_dropped_duplicate);
    (void)mvwprintw(m_win_server, 8, 1, "expired: %4u", stats.msgs_dropped_expired);

    (void)mvwhline(m_win_server, 10, 0, ACS_HLINE, getmaxx(m_win_server));
    (void)wattron(m_win_server, A_BOLD);
    (void)mvwprintw(m_win_server, 11, 1, "IMPAIRMENT");
    (void)wattroff(m_win_server, A_BOLD);
    (void)mvwprintw(m_win_server, 12, 1, "loss:    %4u", stats.impairment.loss_drops);
    (void)mvwprintw(m_win_server, 13, 1, "reorder: %4u", stats.impairment.reorder_buffered);
    (void)mvwprintw(m_win_server, 14, 1, "dups:    %4u", stats.impairment.duplicate_injects);
    (void)mvwprintw(m_win_server, 15, 1, "part:    %4u", stats.impairment.partition_drops);

    (void)wborder(m_win_server, ACS_VLINE, ACS_VLINE,
                  ACS_HLINE, ACS_HLINE,
                  ACS_ULCORNER, ACS_URCORNER,
                  ACS_LLCORNER, ACS_LRCORNER);
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::draw_wire_panel() — private
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::draw_wire_panel()
{
    NEVER_COMPILED_OUT_ASSERT(m_win_wire  != nullptr);            // pre: window valid
    NEVER_COMPILED_OUT_ASSERT(m_wire_count <= WIRE_HISTORY_MAX);  // pre: count bounded

    const int panel_h    = getmaxy(m_win_wire);
    const int panel_w    = getmaxx(m_win_wire);
    const int inner_w    = panel_w - 2;  // exclude left/right border columns

    // Four lifeline columns at 1/8, 3/8, 5/8, 7/8 of inner width.
    const int cli_col     = 1 + inner_w / 8;
    const int cli_imp_col = 1 + (3 * inner_w) / 8;
    const int srv_imp_col = 1 + (5 * inner_w) / 8;
    const int srv_col     = 1 + (7 * inner_w) / 8;

    // Row 1: actor header labels centred on each lifeline.
    // Row 0 is the top border drawn by wborder() at the end; draw labels at row 1.
    // Each label is 6 chars wide; offset -3 centres it on the lifeline column.
    (void)wattron(m_win_wire, A_BOLD);
    (void)mvwprintw(m_win_wire, 1, cli_col     - 3, "CLIENT");
    (void)mvwprintw(m_win_wire, 1, cli_imp_col - 3, "C.IMPR");
    (void)mvwprintw(m_win_wire, 1, srv_imp_col - 3, "S.IMPR");
    (void)mvwprintw(m_win_wire, 1, srv_col     - 3, "SERVER");
    (void)wattroff(m_win_wire, A_BOLD);

    // Row 2: horizontal separator.
    (void)mvwhline(m_win_wire, 2, 0, ACS_HLINE, panel_w);

    // Rows 3..panel_h-2 are available for sequence diagram entries.
    const int available_rows = panel_h - 4;  // top border + header + separator + bottom border
    if (available_rows <= 0) {
        (void)wborder(m_win_wire, ACS_VLINE, ACS_VLINE,
                      ACS_HLINE, ACS_HLINE,
                      ACS_ULCORNER, ACS_URCORNER,
                      ACS_LLCORNER, ACS_LRCORNER);
        return;
    }

    const uint32_t show_count =
        (m_wire_count < static_cast<uint32_t>(available_rows))
        ? m_wire_count
        : static_cast<uint32_t>(available_rows);

    const uint32_t oldest =
        (m_wire_head + WIRE_HISTORY_MAX - show_count) % WIRE_HISTORY_MAX;

    // Power of 10 Rule 2: bounded by show_count <= WIRE_HISTORY_MAX (compile-time const).
    for (uint32_t i = 0U; i < show_count; ++i) {
        const uint32_t slot = (oldest + i) % WIRE_HISTORY_MAX;
        const int      row  = 3 + static_cast<int>(i);
        draw_seqdiag_row(row, cli_col, cli_imp_col, srv_imp_col, srv_col, m_wire[slot]);
    }

    // Fill remaining rows with bare lifelines so the diagram looks continuous.
    // Power of 10 Rule 2 deviation: bounded by panel_h (terminal height, runtime const).
    for (int r = 3 + static_cast<int>(show_count); r < panel_h - 1; ++r) {
        (void)mvwaddch(m_win_wire, r, cli_col,     static_cast<chtype>('|'));
        (void)mvwaddch(m_win_wire, r, cli_imp_col, static_cast<chtype>('|'));
        (void)mvwaddch(m_win_wire, r, srv_imp_col, static_cast<chtype>('|'));
        (void)mvwaddch(m_win_wire, r, srv_col,     static_cast<chtype>('|'));
    }

    (void)wborder(m_win_wire, ACS_VLINE, ACS_VLINE,
                  ACS_HLINE, ACS_HLINE,
                  ACS_ULCORNER, ACS_URCORNER,
                  ACS_LLCORNER, ACS_LRCORNER);
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::draw_seqdiag_row() — private
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::draw_seqdiag_row(int row,
                                        int cli_col, int cli_imp_col,
                                        int srv_imp_col, int srv_col,
                                        const WireEntry& entry) const
{
    NEVER_COMPILED_OUT_ASSERT(m_win_wire != nullptr);      // pre: window valid
    NEVER_COMPILED_OUT_ASSERT(cli_col < srv_col);          // pre: valid lifeline order

    // Always draw all four vertical lifeline characters.
    (void)mvwaddch(m_win_wire, row, cli_col,     static_cast<chtype>('|'));
    (void)mvwaddch(m_win_wire, row, cli_imp_col, static_cast<chtype>('|'));
    (void)mvwaddch(m_win_wire, row, srv_imp_col, static_cast<chtype>('|'));
    (void)mvwaddch(m_win_wire, row, srv_col,     static_cast<chtype>('|'));

    if (!entry.active) {
        return;
    }

    const int full_span  = srv_col - cli_col - 1;       // CLI → SRV inner chars
    const int label_len  = static_cast<int>(strlen(entry.text));

    (void)wattron(m_win_wire, COLOR_PAIR(entry.color_pair));

    switch (entry.dir) {
        case DIR_CLI_TO_SRV:
            // Full span: CLI → SRV, label centred.
            (void)mvwhline(m_win_wire, row, cli_col + 1,
                           static_cast<chtype>('-'), full_span - 1);
            (void)mvwaddch(m_win_wire, row, srv_col - 1, static_cast<chtype>('>'));
            if (label_len < full_span - 1) {
                (void)mvwprintw(m_win_wire, row,
                                cli_col + 1 + (full_span - 1 - label_len) / 2,
                                "%.*s", full_span - 1, entry.text);
            }
            break;

        case DIR_SRV_TO_CLI:
            // Full span: SRV → CLI, label centred.
            (void)mvwhline(m_win_wire, row, cli_col + 2,
                           static_cast<chtype>('-'), full_span - 1);
            (void)mvwaddch(m_win_wire, row, cli_col + 1, static_cast<chtype>('<'));
            if (label_len < full_span - 1) {
                (void)mvwprintw(m_win_wire, row,
                                cli_col + 2 + (full_span - 1 - label_len) / 2,
                                "%.*s", full_span - 1, entry.text);
            }
            break;

        case DIR_CLI_IMPAIR: {
            // Outbound loss: CLI → C.IMPR then X (never left client impairment).
            const int span_ci = cli_imp_col - cli_col - 1;
            (void)mvwhline(m_win_wire, row, cli_col + 1,
                           static_cast<chtype>('-'), span_ci);
            (void)mvwaddch(m_win_wire, row, cli_imp_col, static_cast<chtype>('X'));
            (void)mvwprintw(m_win_wire, row, cli_imp_col + 1,
                            "%.*s", srv_col - cli_imp_col - 2, entry.text);
            break;
        }

        case DIR_SRV_IMPAIR: {
            // Inbound partition: arrow spans CLI → S.IMPR then X.
            const int span_cs = srv_imp_col - cli_col - 1;
            (void)mvwhline(m_win_wire, row, cli_col + 1,
                           static_cast<chtype>('-'), span_cs);
            (void)mvwaddch(m_win_wire, row, srv_imp_col, static_cast<chtype>('X'));
            (void)mvwprintw(m_win_wire, row, srv_imp_col + 1,
                            "%.*s", srv_col - srv_imp_col - 2, entry.text);
            break;
        }

        case DIR_DROP: {
            // Unknown drop: centred label across full span.
            const int lx = cli_col + 1 + (full_span - label_len) / 2;
            (void)mvwprintw(m_win_wire, row,
                            (lx > cli_col + 1) ? lx : cli_col + 1,
                            "%-*.*s", full_span, full_span, entry.text);
            break;
        }

        case DIR_LOCAL:
        default:
            // Local events near client lifeline.
            (void)mvwprintw(m_win_wire, row, cli_col + 1,
                            "%-*.*s", full_span, full_span, entry.text);
            break;
    }

    (void)wattroff(m_win_wire, COLOR_PAIR(entry.color_pair));
}

// ─────────────────────────────────────────────────────────────────────────────
// NcursesRenderer::draw_status_bar() — private
// ─────────────────────────────────────────────────────────────────────────────

void NcursesRenderer::draw_status_bar(const StepController& ctrl)
{
    NEVER_COMPILED_OUT_ASSERT(m_win_status != nullptr);  // pre: window valid
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // pre: renderer ready

    const char* mode_str = (ctrl.get_mode() == MODE_STEP) ? "STEP" : "RUN ";
    const char* done_str = ctrl.scenario_done() ? " [DONE]" : "       ";

    (void)wattron(m_win_status, A_REVERSE);
    (void)mvwprintw(m_win_status, 0, 0,
        " [SPACE] step  [r] run/step  [i] impair  [n] restart  [q] quit"
        "  Mode:%-4s  Profile:%-12s  Msg:%u/%u%s",
        mode_str,
        ctrl.profile_name(),
        ctrl.scenario_idx(),
        SCENARIO_COUNT,
        done_str);
    (void)wclrtoeol(m_win_status);
    (void)wattroff(m_win_status, A_REVERSE);
}
