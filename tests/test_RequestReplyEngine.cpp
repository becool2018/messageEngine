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
 * @file test_RequestReplyEngine.cpp
 * @brief Unit tests for RequestReplyEngine: request/response correlation,
 *        timeout sweep, full-table, and stash-overflow handling.
 *
 * Uses two LocalSimHarness + DeliveryEngine pairs wired bidirectionally
 * for realistic end-to-end flow without real sockets.
 *
 * Rules applied (tests/ relaxations per CLAUDE.md §1b):
 *   - Power of 10: fixed buffers, bounded loops, ≥1 assertion per test
 *     function (Rule 5 relaxed to one assertion per test per §1b).
 *   - MISRA advisory rules relaxed where test framework requires it.
 *   - STL containers NOT used (exemption not needed here).
 *   - Dynamic allocation (assert, printf) via test harness is permitted.
 *   - No raw assert() in production paths; assert() used in test body only.
 *
 * Verifies: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
 */
// Verifies: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/ChannelConfig.hpp"
#include "core/DeliveryEngine.hpp"
#include "core/RequestReplyEngine.hpp"
#include "platform/LocalSimHarness.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static const uint64_t NOW_US        = 1000000ULL;   ///< fixed test clock (1 s)
static const uint64_t TIMEOUT_5S    = 5000000ULL;   ///< 5 second request timeout

// Node IDs used in tests
static const NodeId NODE_A = 1U;
static const NodeId NODE_B = 2U;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: create a LOCAL_SIM TransportConfig for a given node
// ─────────────────────────────────────────────────────────────────────────────
static void make_sim_config(TransportConfig& cfg, NodeId node_id)
{
    transport_config_default(cfg);
    cfg.kind                          = TransportKind::LOCAL_SIM;
    cfg.local_node_id                 = node_id;
    cfg.is_server                     = false;
    cfg.channels[0].max_retries       = 3U;
    cfg.channels[0].retry_backoff_ms  = 50U;
    cfg.channels[0].recv_timeout_ms   = 500U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: set up two linked (harness, engine, rre) nodes
//   Node A: harness_a / engine_a / rre_a  (local_node = NODE_A)
//   Node B: harness_b / engine_b / rre_b  (local_node = NODE_B)
// ─────────────────────────────────────────────────────────────────────────────
static void setup_two_nodes(LocalSimHarness&    harness_a,
                             DeliveryEngine&     engine_a,
                             RequestReplyEngine& rre_a,
                             LocalSimHarness&    harness_b,
                             DeliveryEngine&     engine_b,
                             RequestReplyEngine& rre_b)
{
    TransportConfig cfg_a;
    make_sim_config(cfg_a, NODE_A);
    Result ra = harness_a.init(cfg_a);
    assert(ra == Result::OK);

    TransportConfig cfg_b;
    make_sim_config(cfg_b, NODE_B);
    Result rb = harness_b.init(cfg_b);
    assert(rb == Result::OK);

    // Bidirectional link: messages sent from A appear in B's receive queue, and vice versa.
    harness_a.link(&harness_b);
    harness_b.link(&harness_a);

    engine_a.init(&harness_a, cfg_a.channels[0], NODE_A);
    engine_b.init(&harness_b, cfg_b.channels[0], NODE_B);

    rre_a.init(engine_a, NODE_A);
    rre_b.init(engine_b, NODE_B);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: basic_request_response
// Verifies: REQ-3.2.4, REQ-3.3.3
//
// Send request from A, receive_request on B, send_response from B,
// receive_response on A; verify payload round-trips.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_basic_request_response()
{
    // Verifies: REQ-3.2.4, REQ-3.3.3
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // A sends request
    const uint8_t req_data[] = {0x01U, 0x02U, 0x03U};
    uint64_t cid = 0U;
    Result r = rrea.send_request(NODE_B, req_data, 3U,
                                 TIMEOUT_5S, NOW_US, cid);
    assert(r == Result::OK);
    assert(cid != 0U);

    // B receives request
    uint8_t  rx_req[256];
    uint32_t rx_req_len = 0U;
    NodeId   rx_src     = 0U;
    uint64_t rx_cid     = 0U;
    r = rreb.receive_request(rx_req, 256U, rx_req_len, rx_src, rx_cid,
                             NOW_US);
    assert(r == Result::OK);
    assert(rx_src == NODE_A);
    assert(rx_req_len == 3U);
    assert(rx_cid != 0U);
    assert(rx_req[0] == 0x01U);
    assert(rx_req[1] == 0x02U);
    assert(rx_req[2] == 0x03U);

    // B sends response
    const uint8_t resp_data[] = {0xAAU, 0xBBU};
    r = rreb.send_response(NODE_A, rx_cid, resp_data, 2U, NOW_US);
    assert(r == Result::OK);

    // A receives response
    uint8_t  rx_resp[256];
    uint32_t rx_resp_len = 0U;
    r = rrea.receive_response(cid, rx_resp, 256U, rx_resp_len, NOW_US);
    assert(r == Result::OK);
    assert(rx_resp_len == 2U);
    assert(rx_resp[0] == 0xAAU);
    assert(rx_resp[1] == 0xBBU);

    ha.close();
    hb.close();
    printf("PASS: test_rre_basic_request_response\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: response_correlation
// Verifies: REQ-3.2.4, REQ-3.3.3
//
// Send two requests from A to B with different payloads.
// Verify each response matches the correct request's correlation ID.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_response_correlation()
{
    // Verifies: REQ-3.2.4, REQ-3.3.3
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // A sends request 1
    const uint8_t req1[] = {0x11U};
    uint64_t cid1 = 0U;
    Result r = rrea.send_request(NODE_B, req1, 1U, TIMEOUT_5S, NOW_US, cid1);
    assert(r == Result::OK);

    // A sends request 2
    const uint8_t req2[] = {0x22U};
    uint64_t cid2 = 0U;
    r = rrea.send_request(NODE_B, req2, 1U, TIMEOUT_5S, NOW_US, cid2);
    assert(r == Result::OK);

    // correlation IDs must be distinct
    assert(cid1 != cid2);

    // B receives both requests
    uint8_t  rx1[64];
    uint32_t rx1_len = 0U;
    NodeId   rx1_src = 0U;
    uint64_t rx1_cid = 0U;
    r = rreb.receive_request(rx1, 64U, rx1_len, rx1_src, rx1_cid, NOW_US);
    assert(r == Result::OK);

    uint8_t  rx2[64];
    uint32_t rx2_len = 0U;
    NodeId   rx2_src = 0U;
    uint64_t rx2_cid = 0U;
    r = rreb.receive_request(rx2, 64U, rx2_len, rx2_src, rx2_cid, NOW_US);
    assert(r == Result::OK);

    // B echoes response 2 first, then response 1 (reversed order test)
    const uint8_t resp2[] = {0xCCU};
    r = rreb.send_response(NODE_A, rx2_cid, resp2, 1U, NOW_US);
    assert(r == Result::OK);

    const uint8_t resp1[] = {0xDDU};
    r = rreb.send_response(NODE_A, rx1_cid, resp1, 1U, NOW_US);
    assert(r == Result::OK);

    // A polls for response to cid1 — should get the resp1 payload
    uint8_t  out1[64];
    uint32_t out1_len = 0U;
    r = rrea.receive_response(cid1, out1, 64U, out1_len, NOW_US);
    assert(r == Result::OK);
    assert(out1_len == 1U);
    assert(out1[0] == 0xDDU);

    // A polls for response to cid2 — should get the resp2 payload
    uint8_t  out2[64];
    uint32_t out2_len = 0U;
    r = rrea.receive_response(cid2, out2, 64U, out2_len, NOW_US);
    assert(r == Result::OK);
    assert(out2_len == 1U);
    assert(out2[0] == 0xCCU);

    ha.close();
    hb.close();
    printf("PASS: test_rre_response_correlation\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: response_timeout
// Verifies: REQ-3.2.4, REQ-3.3.2
//
// Send request, never send response; call sweep_timeouts() after timeout;
// verify pending slot freed.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_response_timeout()
{
    // Verifies: REQ-3.2.4, REQ-3.3.2
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Use a short timeout: expires at NOW_US + 1000
    const uint8_t req[] = {0x55U};
    uint64_t cid = 0U;
    Result r = rrea.send_request(NODE_B, req, 1U,
                                 1000ULL /*timeout_us*/, NOW_US, cid);
    assert(r == Result::OK);
    assert(cid != 0U);

    // No response sent.  Advance time past the expiry.
    uint64_t later_us = NOW_US + 2000ULL;
    uint32_t freed = rrea.sweep_timeouts(later_us);
    assert(freed == 1U);

    // receive_response should now return ERR_INVALID (slot freed)
    uint8_t  out[64];
    uint32_t out_len = 0U;
    r = rrea.receive_response(cid, out, 64U, out_len, later_us);
    assert(r == Result::ERR_INVALID);

    ha.close();
    hb.close();
    printf("PASS: test_rre_response_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: pending_table_full
// Verifies: REQ-3.2.4
//
// Fill all MAX_PENDING_REQUESTS slots; verify next send_request returns ERR_FULL.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_pending_table_full()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    const uint8_t dummy[] = {0x00U};
    uint64_t cids[RequestReplyEngine::MAX_PENDING_REQUESTS];

    // Fill all slots
    for (uint32_t i = 0U; i < RequestReplyEngine::MAX_PENDING_REQUESTS; ++i) {
        cids[i] = 0U;
        Result r = rrea.send_request(NODE_B, dummy, 1U,
                                     TIMEOUT_5S, NOW_US, cids[i]);
        assert(r == Result::OK);
    }

    // Next send_request must return ERR_FULL
    uint64_t extra_cid = 0U;
    Result r = rrea.send_request(NODE_B, dummy, 1U,
                                 TIMEOUT_5S, NOW_US, extra_cid);
    assert(r == Result::ERR_FULL);

    ha.close();
    hb.close();
    printf("PASS: test_rre_pending_table_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: unmatched_response_stash
// Verifies: REQ-3.2.4
//
// Inject a RESPONSE envelope (via rre_b.send_response) for an unknown
// correlation ID into rre_a's engine.  Verify it is silently dropped and
// receive_response for that ID returns ERR_INVALID (not a crash).
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_unmatched_response_stash()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // B sends a response for a correlation_id that A never sent as a request.
    const uint64_t UNKNOWN_CID = 0xDEADBEEFCAFEU;
    const uint8_t  resp_data[] = {0x42U};
    Result r = rreb.send_response(NODE_A, UNKNOWN_CID, resp_data, 1U, NOW_US);
    assert(r == Result::OK);  // send itself is OK; drop happens on A's side

    // A attempts to receive the response for the unknown cid — must not crash
    uint8_t  out[64];
    uint32_t out_len = 0U;
    // receive_response for an ID we never registered returns ERR_INVALID
    r = rrea.receive_response(UNKNOWN_CID, out, 64U, out_len, NOW_US);
    assert(r == Result::ERR_INVALID);

    ha.close();
    hb.close();
    printf("PASS: test_rre_unmatched_response_stash\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: receive_request_then_reply
// Verifies: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
//
// B's receive_request returns the correct app payload; send_response
// constructs and delivers the envelope; A's receive_response retrieves it.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_receive_request_then_reply()
{
    // Verifies: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Build a multi-byte request payload
    const uint8_t req_data[] = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U};
    uint64_t cid = 0U;
    Result r = rrea.send_request(NODE_B, req_data, 5U,
                                 TIMEOUT_5S, NOW_US, cid);
    assert(r == Result::OK);

    // B polls — request arrives
    uint8_t  rbuf[256];
    uint32_t rlen = 0U;
    NodeId   rsrc = 0U;
    uint64_t rcid = 0U;
    r = rreb.receive_request(rbuf, 256U, rlen, rsrc, rcid, NOW_US);
    assert(r == Result::OK);
    assert(rlen == 5U);
    assert(rsrc == NODE_A);
    assert(rbuf[0] == 0x10U);
    assert(rbuf[4] == 0x50U);

    // B replies with a 3-byte payload
    const uint8_t reply_data[] = {0xE1U, 0xE2U, 0xE3U};
    r = rreb.send_response(rsrc, rcid, reply_data, 3U, NOW_US);
    assert(r == Result::OK);

    // A retrieves response
    uint8_t  obuf[256];
    uint32_t olen = 0U;
    r = rrea.receive_response(cid, obuf, 256U, olen, NOW_US);
    assert(r == Result::OK);
    assert(olen == 3U);
    assert(obuf[0] == 0xE1U);
    assert(obuf[1] == 0xE2U);
    assert(obuf[2] == 0xE3U);

    ha.close();
    hb.close();
    printf("PASS: test_rre_receive_request_then_reply\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: request_stash_full
// Verifies: REQ-3.2.4
//
// Flood inbound with more requests than MAX_STASH_SIZE from B to A.
// Verify: no crash, and subsequent receive_request calls return ERR_EMPTY
// after the stash is drained.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_request_stash_full()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Send MAX_STASH_SIZE + 2 requests from B to A.
    // The extra ones are expected to be dropped gracefully.
    const uint32_t FLOOD = RequestReplyEngine::MAX_STASH_SIZE + 2U;
    const uint8_t  data[] = {0xFFU};

    for (uint32_t i = 0U; i < FLOOD; ++i) {
        uint64_t cid = 0U;
        // B sends request to A (roles reversed from the usual test)
        Result r = rreb.send_request(NODE_A, data, 1U,
                                     TIMEOUT_5S, NOW_US, cid);
        // We accept both OK and ERR_FULL (B's own pending table may also fill)
        (void)r;
    }

    // A drains its stash via receive_request; must not crash.
    // At most MAX_STASH_SIZE requests are buffered; extras were dropped.
    uint32_t received = 0U;
    for (uint32_t i = 0U; i < RequestReplyEngine::MAX_STASH_SIZE + 4U; ++i) {
        uint8_t  buf[64];
        uint32_t blen = 0U;
        NodeId   bsrc = 0U;
        uint64_t bcid = 0U;
        Result r = rrea.receive_request(buf, 64U, blen, bsrc, bcid, NOW_US);
        if (r == Result::ERR_EMPTY) {
            break;
        }
        assert(r == Result::OK);
        ++received;
    }

    // Received count must be bounded by stash capacity (graceful drop).
    assert(received <= RequestReplyEngine::MAX_STASH_SIZE);

    // One more receive_request must return ERR_EMPTY (stash drained).
    uint8_t  final_buf[64];
    uint32_t final_len = 0U;
    NodeId   final_src = 0U;
    uint64_t final_cid = 0U;
    Result r = rrea.receive_request(final_buf, 64U, final_len,
                                    final_src, final_cid, NOW_US);
    assert(r == Result::ERR_EMPTY);

    ha.close();
    hb.close();
    printf("PASS: test_rre_request_stash_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: non_rr_passthrough
// Verifies: REQ-3.2.4
//
// Send a plain DATA envelope (no RRHeader) from B to A's underlying engine.
// Verify that receive_non_rr() on A returns the frame (FIFO, correct payload).
// Confirms Issue 4 fix: receive_non_rr() calls pump_inbound() internally.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_non_rr_passthrough()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;
    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Build a plain DATA envelope (no RR header — raw application payload).
    MessageEnvelope raw_env;
    envelope_init(raw_env);
    raw_env.message_type     = MessageType::DATA;
    raw_env.source_id        = NODE_B;
    raw_env.destination_id   = NODE_A;
    raw_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    raw_env.payload_length      = 4U;
    raw_env.payload[0]       = 0xCAU;
    raw_env.payload[1]       = 0xFEU;
    raw_env.payload[2]       = 0xBAU;
    raw_env.payload[3]       = 0xBEU;

    // Send via engine_b (bypasses RRE — raw DATA, no correlation header).
    Result s = eb.send(raw_env, NOW_US);
    assert(s == Result::OK);

    // receive_non_rr() on A must find it (Issue 4 fix: it calls pump_inbound).
    MessageEnvelope out;
    envelope_init(out);
    Result r = rrea.receive_non_rr(out, NOW_US);
    assert(r == Result::OK);
    assert(out.payload_length == 4U);
    assert(out.payload[0] == 0xCAU);
    assert(out.payload[1] == 0xFEU);

    ha.close();
    hb.close();
    printf("PASS: test_rre_non_rr_passthrough\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: non_rr_fifo_order
// Verifies: REQ-3.2.4
//
// Send three plain DATA frames from B to A. Verify receive_non_rr() returns
// them in FIFO order (first in, first out).
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_non_rr_fifo_order()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;
    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    static const uint32_t FRAME_COUNT = 3U;
    static const uint8_t  MARKERS[FRAME_COUNT] = { 0x11U, 0x22U, 0x33U };

    // Send three raw DATA frames in order.
    for (uint32_t i = 0U; i < FRAME_COUNT; ++i) {
        MessageEnvelope raw_env;
        envelope_init(raw_env);
        raw_env.message_type      = MessageType::DATA;
        raw_env.source_id         = NODE_B;
        raw_env.destination_id    = NODE_A;
        raw_env.reliability_class = ReliabilityClass::BEST_EFFORT;
        raw_env.payload_length       = 1U;
        raw_env.payload[0]        = MARKERS[i];
        Result s = eb.send(raw_env, NOW_US + static_cast<uint64_t>(i));
        assert(s == Result::OK);
    }

    // Receive and verify FIFO order.
    for (uint32_t i = 0U; i < FRAME_COUNT; ++i) {
        MessageEnvelope out;
        envelope_init(out);
        Result r = rrea.receive_non_rr(out, NOW_US);
        assert(r == Result::OK);
        assert(out.payload[0] == MARKERS[i]);
    }

    // Stash must be empty after draining all three.
    MessageEnvelope empty_out;
    envelope_init(empty_out);
    Result rem = rrea.receive_non_rr(empty_out, NOW_US);
    assert(rem == Result::ERR_EMPTY);

    ha.close();
    hb.close();
    printf("PASS: test_rre_non_rr_fifo_order\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: non_rr_stash_overflow
// Verifies: REQ-3.2.4
//
// Fill the non-RR stash to MAX_STASH_SIZE then send one more. Verify that
// overflow is dropped gracefully and the original MAX_STASH_SIZE frames survive.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_non_rr_stash_overflow()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;
    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Send MAX_STASH_SIZE + 1 raw DATA frames from B to A.
    // Strategy: trigger pump_inbound() while the stash is already at capacity
    // so the (MAX_STASH_SIZE + 1)th frame is dropped.
    //
    // Step 1: send MAX_STASH_SIZE frames and saturate the stash by calling
    // receive_request() (which calls pump_inbound()) without consuming from
    // the non-RR stash. This fills m_non_rr_stash to MAX_STASH_SIZE.
    static const uint32_t CAP = RequestReplyEngine::MAX_STASH_SIZE;
    for (uint32_t i = 0U; i < CAP; ++i) {
        MessageEnvelope raw_env;
        envelope_init(raw_env);
        raw_env.message_type      = MessageType::DATA;
        raw_env.source_id         = NODE_B;
        raw_env.destination_id    = NODE_A;
        raw_env.reliability_class = ReliabilityClass::BEST_EFFORT;
        raw_env.payload_length    = 1U;
        raw_env.payload[0]        = static_cast<uint8_t>(i & 0xFFU);
        Result s = eb.send(raw_env, NOW_US + static_cast<uint64_t>(i));
        assert(s == Result::OK);
    }
    // receive_request() triggers pump_inbound(), filling non-RR stash to CAP.
    uint8_t  req_buf[64];
    uint32_t req_len  = 0U;
    NodeId   req_src  = 0U;
    uint64_t req_cid  = 0U;
    // Returns ERR_EMPTY because the frames have no RR header — but the pump runs.
    (void)rrea.receive_request(req_buf, 64U, req_len, req_src, req_cid, NOW_US);

    // Step 2: send one more non-RR frame and trigger pump again.
    // The stash is now full so this frame must be dropped (WARNING_LO).
    {
        MessageEnvelope extra_env;
        envelope_init(extra_env);
        extra_env.message_type      = MessageType::DATA;
        extra_env.source_id         = NODE_B;
        extra_env.destination_id    = NODE_A;
        extra_env.reliability_class = ReliabilityClass::BEST_EFFORT;
        extra_env.payload_length    = 1U;
        extra_env.payload[0]        = 0xFFU;
        Result s = eb.send(extra_env, NOW_US + static_cast<uint64_t>(CAP));
        assert(s == Result::OK);
    }
    // Another receive_request() pump — extra frame dropped, stash still at CAP.
    (void)rrea.receive_request(req_buf, 64U, req_len, req_src, req_cid, NOW_US);

    // Step 3: drain the non-RR stash; must return exactly CAP frames.
    uint32_t received = 0U;
    for (uint32_t i = 0U; i <= CAP; ++i) {
        MessageEnvelope out;
        envelope_init(out);
        Result r = rrea.receive_non_rr(out, NOW_US);
        if (r == Result::OK) {
            ++received;
        } else {
            assert(r == Result::ERR_EMPTY);
            break;
        }
    }
    assert(received == CAP);   // exactly CAP; the extra frame was dropped

    ha.close();
    hb.close();
    printf("PASS: test_rre_non_rr_stash_overflow\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: zero_cid_rejected
// Verifies: REQ-3.2.4
//
// A DATA frame whose RRHeader has correlation_id == 0 must be rejected by
// parse_rr_header() and routed to the non-RR stash rather than crashing the
// always-on assertion inside handle_inbound_request / handle_inbound_response.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_zero_cid_rejected()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;
    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Build a DATA envelope whose payload starts with a malformed RRHeader:
    // kind=REQUEST(0), cid=0 (all zero bytes 1-8), pad=0.
    // Any correlation_id of 0 must be rejected per the zero-cid fix.
    MessageEnvelope raw_env;
    envelope_init(raw_env);
    raw_env.message_type      = MessageType::DATA;
    raw_env.source_id         = NODE_B;
    raw_env.destination_id    = NODE_A;
    raw_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    raw_env.expiry_time_us    = NOW_US + 5000000ULL;
    // RRHeader bytes (12 bytes): kind=0, cid=0x0000000000000000, pad=0
    raw_env.payload[0]  = 0x00U;   // kind = REQUEST
    raw_env.payload[1]  = 0x00U;   // cid byte 0 (MSB)
    raw_env.payload[2]  = 0x00U;
    raw_env.payload[3]  = 0x00U;
    raw_env.payload[4]  = 0x00U;
    raw_env.payload[5]  = 0x00U;
    raw_env.payload[6]  = 0x00U;
    raw_env.payload[7]  = 0x00U;
    raw_env.payload[8]  = 0x00U;   // cid byte 7 (LSB) → cid = 0
    raw_env.payload[9]  = 0x00U;   // pad[0]
    raw_env.payload[10] = 0x00U;   // pad[1]
    raw_env.payload[11] = 0x00U;   // pad[2]
    raw_env.payload_length = 12U;
    Result s = eb.send(raw_env, NOW_US);
    assert(s == Result::OK);

    // pump_inbound() should reject the frame (cid==0) and stash it as non-RR.
    uint8_t  req_buf[64];
    uint32_t req_len = 0U;
    NodeId   req_src = 0U;
    uint64_t req_cid = 0U;
    Result rr = rrea.receive_request(req_buf, 64U, req_len, req_src, req_cid, NOW_US);
    assert(rr == Result::ERR_EMPTY);  // not a valid RR request

    // Frame must appear in the non-RR stash (not silently dropped).
    MessageEnvelope stashed;
    envelope_init(stashed);
    Result nr = rrea.receive_non_rr(stashed, NOW_US);
    assert(nr == Result::OK);
    assert(stashed.payload_length == 12U);

    ha.close();
    hb.close();
    printf("PASS: test_rre_zero_cid_rejected\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: nonzero_pad_rejected
// Verifies: REQ-3.2.4
//
// A DATA frame whose RRHeader has a non-zero reserved padding byte must be
// rejected by parse_rr_header() and routed to the non-RR stash.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_nonzero_pad_rejected()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;
    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Build a DATA envelope with a valid kind and non-zero cid, but pad[0] = 0xAA.
    MessageEnvelope raw_env;
    envelope_init(raw_env);
    raw_env.message_type      = MessageType::DATA;
    raw_env.source_id         = NODE_B;
    raw_env.destination_id    = NODE_A;
    raw_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    raw_env.expiry_time_us    = NOW_US + 5000000ULL;
    // RRHeader: kind=REQUEST(0), cid=0x0000000000000001 (valid non-zero), pad[0]=0xAA
    raw_env.payload[0]  = 0x00U;   // kind = REQUEST
    raw_env.payload[1]  = 0x00U;   // cid MSB
    raw_env.payload[2]  = 0x00U;
    raw_env.payload[3]  = 0x00U;
    raw_env.payload[4]  = 0x00U;
    raw_env.payload[5]  = 0x00U;
    raw_env.payload[6]  = 0x00U;
    raw_env.payload[7]  = 0x00U;
    raw_env.payload[8]  = 0x01U;   // cid LSB → cid = 1 (valid)
    raw_env.payload[9]  = 0xAAU;   // pad[0] — non-zero; must be rejected
    raw_env.payload[10] = 0x00U;
    raw_env.payload[11] = 0x00U;
    raw_env.payload_length = 12U;
    Result s = eb.send(raw_env, NOW_US);
    assert(s == Result::OK);

    // pump_inbound() must reject the frame (non-zero pad) and stash it as non-RR.
    uint8_t  req_buf[64];
    uint32_t req_len = 0U;
    NodeId   req_src = 0U;
    uint64_t req_cid = 0U;
    Result rr = rrea.receive_request(req_buf, 64U, req_len, req_src, req_cid, NOW_US);
    assert(rr == Result::ERR_EMPTY);  // not a valid RR request

    // Frame must appear in the non-RR stash.
    MessageEnvelope stashed;
    envelope_init(stashed);
    Result nr = rrea.receive_non_rr(stashed, NOW_US);
    assert(nr == Result::OK);

    ha.close();
    hb.close();
    printf("PASS: test_rre_nonzero_pad_rejected\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: big_endian_cid_encoding
// Verifies: REQ-3.2.4
//
// Verify that the big-endian correlation_id encoding is correctly decoded
// by injecting a DATA frame whose RRHeader carries a known big-endian cid
// value with significant bits in the high bytes (0x0102030405060708).
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_big_endian_cid_encoding()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;
    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Craft a DATA frame whose RRHeader has a known non-trivial big-endian cid.
    // The high bytes (0x01 through 0x04) exercise the MSB-first shift decoding.
    // Expected decoded cid: 0x0102030405060708ULL
    static const uint64_t EXPECTED_CID = 0x0102030405060708ULL;

    MessageEnvelope raw_env;
    envelope_init(raw_env);
    raw_env.message_type      = MessageType::DATA;
    raw_env.source_id         = NODE_B;
    raw_env.destination_id    = NODE_A;
    raw_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    raw_env.expiry_time_us    = NOW_US + 5000000ULL;
    // RRHeader bytes: kind=REQUEST(0), cid big-endian, pad=0
    raw_env.payload[0]  = 0x00U;   // kind = REQUEST
    raw_env.payload[1]  = 0x01U;   // cid byte 0 (MSB)
    raw_env.payload[2]  = 0x02U;
    raw_env.payload[3]  = 0x03U;
    raw_env.payload[4]  = 0x04U;
    raw_env.payload[5]  = 0x05U;
    raw_env.payload[6]  = 0x06U;
    raw_env.payload[7]  = 0x07U;
    raw_env.payload[8]  = 0x08U;   // cid byte 7 (LSB)
    raw_env.payload[9]  = 0x00U;   // pad[0]
    raw_env.payload[10] = 0x00U;   // pad[1]
    raw_env.payload[11] = 0x00U;   // pad[2]
    // Append one byte of app payload after the header
    raw_env.payload[12]    = 0x42U;
    raw_env.payload_length = 13U;
    Result s = eb.send(raw_env, NOW_US);
    assert(s == Result::OK);

    // receive_request() on A's RRE must decode the cid as EXPECTED_CID.
    uint8_t  req_buf[64];
    uint32_t req_len = 0U;
    NodeId   req_src = 0U;
    uint64_t rx_cid  = 0U;
    Result rr = rrea.receive_request(req_buf, 64U, req_len, req_src, rx_cid, NOW_US);
    assert(rr == Result::OK);
    assert(rx_cid == EXPECTED_CID);
    assert(req_len == 1U);
    assert(req_buf[0] == 0x42U);

    ha.close();
    hb.close();
    printf("PASS: test_rre_big_endian_cid_encoding\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main()
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Test 14: request stash FIFO reuse
// Verifies: REQ-3.2.4
//
// After dequeuing the oldest request (slot freed), a subsequent send must not
// overtake requests already buffered in the stash. Demonstrates that the FIFO
// read/write cursors (m_stash_head, m_stash_count) correctly wrap across slot
// reuse: dequeue A; send D; verify dequeue order is B, C, D — not D, B, C.
// ─────────────────────────────────────────────────────────────────────────────
static void test_rre_request_stash_fifo_reuse()
{
    // Verifies: REQ-3.2.4
    LocalSimHarness    ha;
    DeliveryEngine     ea;
    RequestReplyEngine rrea;
    LocalSimHarness    hb;
    DeliveryEngine     eb;
    RequestReplyEngine rreb;

    setup_two_nodes(ha, ea, rrea, hb, eb, rreb);

    // Payload bytes used as unique identifiers for each request
    static const uint8_t PAY_A = 0x0AU;
    static const uint8_t PAY_B = 0x0BU;
    static const uint8_t PAY_C = 0x0CU;
    static const uint8_t PAY_D = 0x0DU;

    // Send requests A, B, C from A to B before B ever polls.
    // All three land in harness_b's receive queue in FIFO wire order.
    uint64_t cid_a = 0U;
    uint64_t cid_b = 0U;
    uint64_t cid_c = 0U;
    Result ra = rrea.send_request(NODE_B, &PAY_A, 1U, TIMEOUT_5S, NOW_US, cid_a);
    assert(ra == Result::OK);
    Result rb = rrea.send_request(NODE_B, &PAY_B, 1U, TIMEOUT_5S, NOW_US, cid_b);
    assert(rb == Result::OK);
    Result rc = rrea.send_request(NODE_B, &PAY_C, 1U, TIMEOUT_5S, NOW_US, cid_c);
    assert(rc == Result::OK);

    // B's first receive_request: pump_inbound drains A, B, C; stash = [A, B, C].
    // Must return A (oldest).
    uint8_t  buf[64U];
    uint32_t blen = 0U;
    NodeId   bsrc = 0U;
    uint64_t bcid = 0U;
    Result r1 = rreb.receive_request(buf, 64U, blen, bsrc, bcid, NOW_US);
    assert(r1 == Result::OK);
    assert(blen == 1U);
    assert(buf[0] == PAY_A);  // Assert: oldest request (A) returned first

    // Now send D after A was dequeued.
    // D must not overtake B or C, which are still in the stash.
    uint64_t cid_d = 0U;
    Result rd = rrea.send_request(NODE_B, &PAY_D, 1U, TIMEOUT_5S, NOW_US, cid_d);
    assert(rd == Result::OK);

    // Dequeue remaining: FIFO order must be B, C, D.
    blen = 0U; bsrc = 0U; bcid = 0U;
    Result r2 = rreb.receive_request(buf, 64U, blen, bsrc, bcid, NOW_US);
    assert(r2 == Result::OK);
    assert(buf[0] == PAY_B);  // Assert: B precedes D (FIFO preserved across slot reuse)

    blen = 0U; bsrc = 0U; bcid = 0U;
    Result r3 = rreb.receive_request(buf, 64U, blen, bsrc, bcid, NOW_US);
    assert(r3 == Result::OK);
    assert(buf[0] == PAY_C);  // Assert: C precedes D

    blen = 0U; bsrc = 0U; bcid = 0U;
    Result r4 = rreb.receive_request(buf, 64U, blen, bsrc, bcid, NOW_US);
    assert(r4 == Result::OK);
    assert(buf[0] == PAY_D);  // Assert: D last

    // Stash must be empty after all four are consumed.
    blen = 0U; bsrc = 0U; bcid = 0U;
    Result r5 = rreb.receive_request(buf, 64U, blen, bsrc, bcid, NOW_US);
    assert(r5 == Result::ERR_EMPTY);

    ha.close();
    hb.close();
    printf("PASS: test_rre_request_stash_fifo_reuse\n");
}

int main()
{
    printf("=== test_RequestReplyEngine ===\n");

    test_rre_basic_request_response();
    test_rre_response_correlation();
    test_rre_response_timeout();
    test_rre_pending_table_full();
    test_rre_unmatched_response_stash();
    test_rre_receive_request_then_reply();
    test_rre_request_stash_full();
    test_rre_non_rr_passthrough();
    test_rre_non_rr_fifo_order();
    test_rre_non_rr_stash_overflow();
    test_rre_zero_cid_rejected();
    test_rre_nonzero_pad_rejected();
    test_rre_big_endian_cid_encoding();
    test_rre_request_stash_fifo_reuse();

    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
