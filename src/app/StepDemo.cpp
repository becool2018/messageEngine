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
 * @file StepDemo.cpp
 * @brief Stepped visual TCP client/server communication demo.
 *
 * Runs a real TCP client and server in a single process on loopback (127.0.0.1:9100).
 * Both DeliveryEngine instances are driven from a single-threaded step controller.
 * The ncurses display shows wire events, delivery stats, and impairment effects in
 * real time. Press SPACE to advance one step, 'r' to run freely, 'i' to cycle
 * impairment profiles, and 'q' to quit.
 *
 * No new APIs or core/platform files are needed — this uses only existing public
 * interfaces: DeliveryEngine::send/receive/pump_retries/sweep_ack_timeouts/
 * poll_event/drain_events/get_stats and TcpBackend::init/close/register_local_id.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds with deviation comments for interactive loops,
 *     >=2 assertions per function, no dynamic allocation, CC<=10.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - F-Prime style: Result returns, Logger, plain structs.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-7.2.5
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
//             REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-7.2.5

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <time.h>

#include "core/Types.hpp"
#include "core/Assert.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/DeliveryEngine.hpp"
#include "core/DeliveryEvent.hpp"
#include "core/Timestamp.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"
#include "platform/TcpBackend.hpp"
#include "app/StepController.hpp"
#include "app/NcursesRenderer.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static const uint32_t DEMO_PORT          = 9100U;
static const NodeId   SERVER_NODE_ID     = 1U;
static const NodeId   CLIENT_NODE_ID     = 2U;
static const uint32_t MAX_SESSIONS       = 100U; ///< Session restart bound
static const uint32_t MAX_TICKS          = 2000U; ///< Max ticks per session
static const uint32_t RECV_TIMEOUT_MS    = 80U;   ///< Per-engine receive timeout
static const uint32_t STARTUP_TIMEOUT_MS = 300U;  ///< Timeout waiting for HELLO
static const uint32_t RUN_SLEEP_MS       = 500U;  ///< Delay between auto-ticks
static const uint32_t EVT_BUF_SIZE       = 16U;   ///< Max events drained per tick
static const uint32_t EXPIRY_MS          = 8000U; ///< Message expiry (8 seconds)
static const uint32_t MAX_STEP_WAIT      = 50000U; ///< Bound on STEP-mode key poll

// ─────────────────────────────────────────────────────────────────────────────
// sleep_ms() — POSIX nanosleep wrapper
// ─────────────────────────────────────────────────────────────────────────────

static void sleep_ms(uint32_t ms)
{
    NEVER_COMPILED_OUT_ASSERT(ms <= 10000U);  // pre: reasonable duration
    NEVER_COMPILED_OUT_ASSERT(ms > 0U);       // pre: positive

    struct timespec ts;
    ts.tv_sec  = static_cast<time_t>(ms / 1000U);
    ts.tv_nsec = static_cast<long>((ms % 1000U) * 1000000L);
    (void)nanosleep(&ts, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_channel_defaults() — configure a channel for the demo
// ─────────────────────────────────────────────────────────────────────────────

static void apply_channel_defaults(ChannelConfig& ch)
{
    NEVER_COMPILED_OUT_ASSERT(ch.channel_id < static_cast<uint8_t>(MAX_CHANNELS)); // pre: valid id
    NEVER_COMPILED_OUT_ASSERT(MAX_CHANNELS > 0U);                                  // pre: non-zero

    channel_config_default(ch, 0U);
    ch.reliability     = ReliabilityClass::RELIABLE_RETRY;
    ch.ordering        = OrderingMode::ORDERED;
    ch.max_retries     = 3U;
    ch.recv_timeout_ms = RECV_TIMEOUT_MS;
    ch.send_timeout_ms = RECV_TIMEOUT_MS;
}

// ─────────────────────────────────────────────────────────────────────────────
// init_server() — bind, listen, and initialize server-side engine
// ─────────────────────────────────────────────────────────────────────────────

static Result init_server(TcpBackend& transport,
                           DeliveryEngine& engine,
                           ImpairProfile profile)
{
    NEVER_COMPILED_OUT_ASSERT(DEMO_PORT > 0U);       // pre: valid port
    NEVER_COMPILED_OUT_ASSERT(SERVER_NODE_ID != 0U); // pre: valid node id

    TransportConfig cfg;
    transport_config_default(cfg);

    cfg.kind          = TransportKind::TCP;
    cfg.is_server     = true;
    cfg.bind_port     = static_cast<uint16_t>(DEMO_PORT);
    cfg.local_node_id = SERVER_NODE_ID;
    cfg.num_channels  = 1U;

    (void)strncpy(cfg.bind_ip, "0.0.0.0", sizeof(cfg.bind_ip) - 1U);
    cfg.bind_ip[sizeof(cfg.bind_ip) - 1U] = '\0';

    apply_channel_defaults(cfg.channels[0]);
    // Server inbound impairment: apply profile so server-side drops are visible.
    StepController::fill_impairment(cfg.channels[0].impairment, profile);

    Result res = transport.init(cfg);
    if (!result_ok(res)) {
        return res;
    }

    engine.init(&transport, cfg.channels[0], SERVER_NODE_ID);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// init_client() — connect and initialize client-side engine
// ─────────────────────────────────────────────────────────────────────────────

static Result init_client(TcpBackend& transport,
                           DeliveryEngine& engine,
                           ImpairProfile profile)
{
    NEVER_COMPILED_OUT_ASSERT(DEMO_PORT > 0U);       // pre: valid port
    NEVER_COMPILED_OUT_ASSERT(CLIENT_NODE_ID != 0U); // pre: valid node id

    TransportConfig cfg;
    transport_config_default(cfg);

    cfg.kind               = TransportKind::TCP;
    cfg.is_server          = false;
    cfg.peer_port          = static_cast<uint16_t>(DEMO_PORT);
    cfg.local_node_id      = CLIENT_NODE_ID;
    cfg.num_channels       = 1U;
    cfg.connect_timeout_ms = 3000U;

    (void)strncpy(cfg.peer_ip, "127.0.0.1", sizeof(cfg.peer_ip) - 1U);
    cfg.peer_ip[sizeof(cfg.peer_ip) - 1U] = '\0';

    apply_channel_defaults(cfg.channels[0]);
    // Client outbound impairment: apply profile.
    StepController::fill_impairment(cfg.channels[0].impairment, profile);

    Result res = transport.init(cfg);
    if (!result_ok(res)) {
        return res;
    }

    engine.init(&transport, cfg.channels[0], CLIENT_NODE_ID);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// wait_for_hello() — let server accept the connection and process HELLO
// ─────────────────────────────────────────────────────────────────────────────

static void wait_for_hello(DeliveryEngine& server_engine)
{
    NEVER_COMPILED_OUT_ASSERT(STARTUP_TIMEOUT_MS > 0U); // pre: valid timeout
    NEVER_COMPILED_OUT_ASSERT(SERVER_NODE_ID != 0U);    // pre: valid id

    uint64_t now_us = timestamp_now_us();
    MessageEnvelope env;
    envelope_init(env);
    // receive() will call poll_clients_once() → accept_clients() + process HELLO.
    // ERR_TIMEOUT is expected (no DATA arrives during startup).
    (void)server_engine.receive(env, STARTUP_TIMEOUT_MS, now_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_scenario_msg() — fill an envelope for the current scenario entry
// ─────────────────────────────────────────────────────────────────────────────

static void build_scenario_msg(MessageEnvelope& env,
                                const ScenarioEntry& entry,
                                uint32_t scenario_idx,
                                uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0U);               // pre: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(scenario_idx < SCENARIO_COUNT); // pre: valid index

    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.destination_id    = SERVER_NODE_ID;
    env.priority          = 0U;
    env.reliability_class = entry.reliability;
    env.timestamp_us      = now_us;
    env.expiry_time_us    = timestamp_deadline_us(now_us, EXPIRY_MS);

    char payload[64] = {};
    int len = snprintf(payload, sizeof(payload), "msg%u [%s]",
                       scenario_idx + 1U, entry.label);
    if (len > 0 && static_cast<uint32_t>(len) < MSG_MAX_PAYLOAD_BYTES) {
        (void)memcpy(env.payload, payload, static_cast<uint32_t>(len));
        env.payload_length = static_cast<uint32_t>(len);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// event_dir() — map a DeliveryEvent kind to a sequence diagram arrow direction
// ─────────────────────────────────────────────────────────────────────────────

static WireDir event_dir(DeliveryEventKind kind, bool from_client)
{
    NEVER_COMPILED_OUT_ASSERT(EVT_BUF_SIZE > 0U);    // pre: valid context
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid context

    switch (kind) {
        case DeliveryEventKind::SEND_OK:
        case DeliveryEventKind::RETRY_FIRED:
            return from_client ? DIR_CLI_TO_SRV : DIR_SRV_TO_CLI;
        case DeliveryEventKind::ACK_RECEIVED:
            return from_client ? DIR_SRV_TO_CLI : DIR_CLI_TO_SRV;
        case DeliveryEventKind::SEND_FAIL:
        case DeliveryEventKind::DUPLICATE_DROP:
        case DeliveryEventKind::EXPIRY_DROP:
        case DeliveryEventKind::MISROUTE_DROP:
            return DIR_DROP;
        case DeliveryEventKind::ACK_TIMEOUT:
        case DeliveryEventKind::RECONNECT_CANCEL:
        default:
            return DIR_LOCAL;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// format_event() — format one DeliveryEvent as a sequence diagram label
// ─────────────────────────────────────────────────────────────────────────────

static void format_event(const DeliveryEvent& ev,
                          char* buf,
                          uint32_t buf_size,
                          int& color_pair_out)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);   // pre: valid buffer
    NEVER_COMPILED_OUT_ASSERT(buf_size > 0U);    // pre: non-zero size

    color_pair_out = COLOR_PAIR_GREEN;

    switch (ev.kind) {
        case DeliveryEventKind::SEND_OK:
            (void)snprintf(buf, buf_size, "DATA  id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_GREEN;
            break;
        case DeliveryEventKind::SEND_FAIL:
            (void)snprintf(buf, buf_size, "FAIL  id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_RED;
            break;
        case DeliveryEventKind::ACK_RECEIVED:
            (void)snprintf(buf, buf_size, "ACK   id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_GREEN;
            break;
        case DeliveryEventKind::RETRY_FIRED:
            (void)snprintf(buf, buf_size, "RETRY id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_YELLOW;
            break;
        case DeliveryEventKind::ACK_TIMEOUT:
            (void)snprintf(buf, buf_size, "TIMEOUT id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_YELLOW;
            break;
        case DeliveryEventKind::DUPLICATE_DROP:
            (void)snprintf(buf, buf_size, "DUP   id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_RED;
            break;
        case DeliveryEventKind::EXPIRY_DROP:
            (void)snprintf(buf, buf_size, "EXPRD id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_RED;
            break;
        case DeliveryEventKind::MISROUTE_DROP:
            (void)snprintf(buf, buf_size, "MSRTE id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_RED;
            break;
        case DeliveryEventKind::RECONNECT_CANCEL:
            (void)snprintf(buf, buf_size, "RECONN id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_RED;
            break;
        case DeliveryEventKind::IMPAIR_DROP:
            // IMPAIR_DROP events come from drain_impair_events(), not here;
            // handle defensively in case the enum is used elsewhere.
            (void)snprintf(buf, buf_size, "IMPAIR id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_RED;
            break;
        default:
            (void)snprintf(buf, buf_size, "UNK   id=%llu",
                           static_cast<unsigned long long>(ev.message_id));
            color_pair_out = COLOR_PAIR_NORMAL;
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// drain_and_display() — drain one engine's event ring into the wire panel
// ─────────────────────────────────────────────────────────────────────────────

static void drain_and_display(DeliveryEngine& engine,
                               bool from_client,
                               NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(EVT_BUF_SIZE > 0U);  // pre: buffer non-zero
    NEVER_COMPILED_OUT_ASSERT(EVT_BUF_SIZE <= 64U); // pre: reasonable bound

    DeliveryEvent evts[EVT_BUF_SIZE];
    const uint32_t n = engine.drain_events(evts, EVT_BUF_SIZE);

    // Power of 10 Rule 2: bounded by EVT_BUF_SIZE (compile-time constant).
    for (uint32_t i = 0U; i < n; ++i) {
        char    text[80]   = {};
        int     color_pair = COLOR_PAIR_NORMAL;
        WireDir dir        = DIR_LOCAL;
        format_event(evts[i], text, static_cast<uint32_t>(sizeof(text)), color_pair);
        dir = event_dir(evts[i].kind, from_client);
        renderer.add_wire_event(text, color_pair, dir);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// drain_impair_events() — drain impairment drop records into the wire panel
// ─────────────────────────────────────────────────────────────────────────────

static void drain_impair_events(TcpBackend& transport,
                                 bool from_client,
                                 NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(EVT_BUF_SIZE > 0U);    // pre: buffer bound positive
    NEVER_COMPILED_OUT_ASSERT(EVT_BUF_SIZE <= 64U);  // pre: reasonable bound

    ImpairDropRecord recs[EVT_BUF_SIZE];
    const uint32_t n = transport.drain_impair_drops(recs, EVT_BUF_SIZE);

    // Power of 10 Rule 2: bounded by EVT_BUF_SIZE (compile-time constant).
    for (uint32_t i = 0U; i < n; ++i) {
        char text[80] = {};
        (void)snprintf(text, sizeof(text), "IMPAIR id=%llu",
                       static_cast<unsigned long long>(recs[i].message_id));
        // Outbound drop = client impairment layer; inbound = server impairment layer.
        const WireDir dir = recs[i].outbound ? DIR_CLI_IMPAIR : DIR_SRV_IMPAIR;
        (void)from_client;  // direction encoded in ImpairDropRecord::outbound
        renderer.add_wire_event(text, COLOR_PAIR_RED, dir);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// do_tick() — advance one step: send, receive, pump, drain events
// ─────────────────────────────────────────────────────────────────────────────

static void do_tick(DeliveryEngine& client_engine,
                    DeliveryEngine& server_engine,
                    TcpBackend& client_transport,
                    TcpBackend& server_transport,
                    StepController& ctrl,
                    NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(SERVER_NODE_ID != 0U);  // pre: valid server id
    NEVER_COMPILED_OUT_ASSERT(RECV_TIMEOUT_MS > 0U);  // pre: valid timeout

    uint64_t now_us = timestamp_now_us();

    // 1. Client sends the next scenario message if any remain.
    if (!ctrl.scenario_done()) {
        MessageEnvelope env;
        build_scenario_msg(env, ctrl.current_scenario(), ctrl.scenario_idx(), now_us);
        Result res = client_engine.send(env, now_us);
        if (result_ok(res)) {
            ctrl.advance_scenario();
            if (ctrl.scenario_done()) {
                // All scenario messages sent — add a visual marker in the wire panel.
                renderer.add_wire_event("─── SCENARIO COMPLETE ───", COLOR_PAIR_CYAN, DIR_LOCAL);
            }
        }
    }

    // 2. Server receives (drains OS socket buffer; processes DATA / ACK).
    {
        MessageEnvelope srv_env;
        envelope_init(srv_env);
        (void)server_engine.receive(srv_env, RECV_TIMEOUT_MS, now_us);
    }

    // 3. Client receives (drains ACKs flowing back from server).
    now_us = timestamp_now_us();
    {
        MessageEnvelope cli_env;
        envelope_init(cli_env);
        (void)client_engine.receive(cli_env, 20U, now_us);
    }

    // 4. Pump retries and sweep ACK timeouts on both engines.
    now_us = timestamp_now_us();
    (void)client_engine.pump_retries(now_us);
    (void)server_engine.pump_retries(now_us);
    (void)client_engine.sweep_ack_timeouts(now_us);
    (void)server_engine.sweep_ack_timeouts(now_us);

    // 5. Drain engine observability events, then impairment drop records.
    drain_and_display(client_engine, true,  renderer);
    drain_and_display(server_engine, false, renderer);
    drain_impair_events(client_transport, true,  renderer);
    drain_impair_events(server_transport, false, renderer);
}

// ─────────────────────────────────────────────────────────────────────────────
// render_stats() — snapshot stats and redraw display
// ─────────────────────────────────────────────────────────────────────────────

static void render_stats(const DeliveryEngine& client_engine,
                          const DeliveryEngine& server_engine,
                          NcursesRenderer& renderer,
                          const StepController& ctrl)
{
    NEVER_COMPILED_OUT_ASSERT(SERVER_NODE_ID != 0U);  // pre: valid state
    NEVER_COMPILED_OUT_ASSERT(CLIENT_NODE_ID != 0U);  // pre: valid state

    DeliveryStats cli_stats;
    DeliveryStats srv_stats;
    delivery_stats_init(cli_stats);
    delivery_stats_init(srv_stats);

    client_engine.get_stats(cli_stats);
    server_engine.get_stats(srv_stats);

    renderer.render(cli_stats, srv_stats, ctrl);
}

// ─────────────────────────────────────────────────────────────────────────────
// wait_for_step() — block until SPACE pressed in STEP mode
// Applies Rule 2 deviation: interactive input loop — bounded by MAX_STEP_WAIT.
// ─────────────────────────────────────────────────────────────────────────────

static bool wait_for_step(StepController& ctrl,
                           const DeliveryEngine& client_engine,
                           const DeliveryEngine& server_engine,
                           NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_STEP_WAIT > 0U);   // pre: bound positive
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: scenario valid

    // Power of 10 Rule 2 deviation: interactive input wait loop —
    // bounded by MAX_STEP_WAIT; terminates on SPACE/q/i or key count exceeded.
    for (uint32_t k = 0U; k < MAX_STEP_WAIT; ++k) {
        int ch = renderer.read_key_blocking();
        ctrl.handle_key(ch);

        if (ctrl.is_quit() || ctrl.is_restart_requested()) {
            return false;
        }
        if (ctrl.get_mode() == MODE_RUN) {
            return true;  // switched to run mode — proceed immediately
        }
        if (ctrl.consume_step()) {
            return true;  // SPACE pressed — advance
        }

        // Any other key: re-render to reflect mode changes.
        render_stats(client_engine, server_engine, renderer, ctrl);
    }

    return false;  // safety exit after MAX_STEP_WAIT keystrokes
}

// ─────────────────────────────────────────────────────────────────────────────
// await_tick_clearance() — wait for permission to execute the next tick
// Returns true if the tick should proceed; false if quit/restart was requested.
// ─────────────────────────────────────────────────────────────────────────────

static bool await_tick_clearance(StepController& ctrl,
                                  const DeliveryEngine& client_engine,
                                  const DeliveryEngine& server_engine,
                                  NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_STEP_WAIT > 0U);   // pre: bound positive
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: scenario valid

    if (ctrl.get_mode() == MODE_STEP) {
        return wait_for_step(ctrl, client_engine, server_engine, renderer);
    }
    // RUN mode: check for key then sleep.
    int ch = renderer.read_key();
    if (ch != ERR) {
        ctrl.handle_key(ch);
    }
    if (ctrl.is_quit() || ctrl.is_restart_requested()) {
        return false;
    }
    sleep_ms(RUN_SLEEP_MS);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// run_session() — main tick loop for one transport session
// ─────────────────────────────────────────────────────────────────────────────

static void run_session(DeliveryEngine& client_engine,
                         DeliveryEngine& server_engine,
                         TcpBackend& client_transport,
                         TcpBackend& server_transport,
                         StepController& ctrl,
                         NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_TICKS > 0U);       // pre: tick bound positive
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: scenario valid

    render_stats(client_engine, server_engine, renderer, ctrl);

    // Power of 10 Rule 2: bounded by MAX_TICKS.
    for (uint32_t tick = 0U; tick < MAX_TICKS; ++tick) {
        if (ctrl.is_quit() || ctrl.is_restart_requested()) {
            break;
        }
        bool proceed = await_tick_clearance(ctrl, client_engine, server_engine, renderer);
        if (!proceed) {
            break;
        }
        do_tick(client_engine, server_engine, client_transport, server_transport,
                ctrl, renderer);
        render_stats(client_engine, server_engine, renderer, ctrl);
        // Pause automatically at scenario end so user can inspect final state.
        if (ctrl.scenario_done() && ctrl.get_mode() == MODE_RUN) {
            ctrl.handle_key('r');
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// wait_for_quit() — block until the user presses 'q' (error recovery path)
// Power of 10 Rule 2 deviation: interactive wait — bounded by MAX_STEP_WAIT.
// ─────────────────────────────────────────────────────────────────────────────

static void wait_for_quit(StepController& ctrl, NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_STEP_WAIT > 0U);   // pre: bound positive
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);  // pre: valid state

    // Power of 10 Rule 2 deviation: interactive input wait — bounded by MAX_STEP_WAIT.
    for (uint32_t w = 0U; w < MAX_STEP_WAIT; ++w) {
        int ch = renderer.read_key_blocking();
        ctrl.handle_key(ch);
        if (ctrl.is_quit()) {
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// run_one_session() — init transports, run scenario, close transports.
// Returns false if a fatal init error occurred and the outer loop should stop.
// ─────────────────────────────────────────────────────────────────────────────

static bool run_one_session(StepController& ctrl, NcursesRenderer& renderer)
{
    NEVER_COMPILED_OUT_ASSERT(DEMO_PORT > 0U);        // pre: valid port
    NEVER_COMPILED_OUT_ASSERT(SCENARIO_COUNT > 0U);   // pre: scenario valid

    // Stack-allocate fresh objects per session (Power of 10 Rule 3: no heap).
    TcpBackend     server_transport;
    TcpBackend     client_transport;
    DeliveryEngine server_engine;
    DeliveryEngine client_engine;

    const ImpairProfile profile = ctrl.get_profile();

    Result res = init_server(server_transport, server_engine, profile);
    if (!result_ok(res)) {
        renderer.add_wire_event("XXX Server init FAILED", COLOR_PAIR_RED,    DIR_LOCAL);
        renderer.add_wire_event("    Press 'q' to quit",  COLOR_PAIR_NORMAL, DIR_LOCAL);
        DeliveryStats dummy;
        delivery_stats_init(dummy);
        renderer.render(dummy, dummy, ctrl);
        wait_for_quit(ctrl, renderer);
        return false;
    }

    res = init_client(client_transport, client_engine, profile);
    if (!result_ok(res)) {
        renderer.add_wire_event("XXX Client init FAILED", COLOR_PAIR_RED, DIR_LOCAL);
        server_transport.close();
        DeliveryStats dummy;
        delivery_stats_init(dummy);
        renderer.render(dummy, dummy, ctrl);
        wait_for_quit(ctrl, renderer);
        return false;
    }

    wait_for_hello(server_engine);
    renderer.add_wire_hello(static_cast<uint32_t>(CLIENT_NODE_ID),
                             static_cast<uint32_t>(SERVER_NODE_ID));

    run_session(client_engine, server_engine,
                client_transport, server_transport,
                ctrl, renderer);

    server_transport.close();
    client_transport.close();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// main()
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);       // pre: valid argc
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr); // pre: valid argv
    (void)argc;
    (void)argv;

    // Suppress logger output to avoid corrupting the ncurses display.
    // Only FATAL events are shown (and they abort the process anyway).
    (void)Logger::init(Severity::FATAL,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

    StepController   ctrl;
    NcursesRenderer  renderer;

    ctrl.init();
    renderer.init();

    // Power of 10 Rule 2: bounded by MAX_SESSIONS.
    for (uint32_t session = 0U; session < MAX_SESSIONS; ++session) {
        if (ctrl.is_quit()) {
            break;
        }
        ctrl.clear_restart();
        ctrl.reset_scenario();
        renderer.clear_wire();

        bool ok = run_one_session(ctrl, renderer);
        if (!ok) {
            break;
        }
        // Brief pause before reopening the port; SO_REUSEADDR handles TIME_WAIT.
        if (!ctrl.is_quit()) {
            sleep_ms(200U);
        }
    }

    renderer.close();
    return 0;
}
