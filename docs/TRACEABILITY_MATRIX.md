# messageEngine Traceability Matrix

Generated from `// Implements:` and `// Verifies:` comments in the codebase.
Regenerate by running: `docs/check_traceability.sh`
Policy: CLAUDE.md §11 / .claude/CLAUDE.md §10

| REQ ID    | Requirement (summary)                              | Implements (src/ file)                                      | Verifies (tests/ file :: test name)                  |
|-----------|----------------------------------------------------|-------------------------------------------------------------|------------------------------------------------------|
| REQ-3.1   | MessageEnvelope standard fields                    | src/core/MessageEnvelope.hpp, src/core/Types.hpp            | test_MessageEnvelope.cpp :: test_envelope_init, test_envelope_valid, test_envelope_copy, test_envelope_is_data |
| REQ-3.2.1 | Message ID generation and comparison               | src/core/MessageId.hpp, src/core/MessageId.cpp              | test_MessageId.cpp :: test_init_seed, test_monotonic_increment, test_wraparound_recovery, test_min_valid_seed |
| REQ-3.2.2 | Timestamps and expiry checking                     | src/core/Timestamp.hpp, src/core/Timestamp.cpp              | test_Timestamp.cpp :: test_expired_false_before_deadline, test_expired_true_at_deadline, test_expired_true_past_deadline, test_expiry_zero_never_expires, test_deadline_arithmetic, test_deadline_zero_duration |
| REQ-3.2.3 | Serialization / deserialization                    | src/core/Serializer.hpp, src/core/Serializer.cpp            | test_Serializer.cpp :: test_serialize_deserialize_basic, test_serialize_buffer_too_small, test_serialize_invalid_envelope, test_deserialize_truncated, test_serialize_max_payload, test_wire_header_size, test_serialize_zero_payload, test_deserialize_header_too_short, test_deserialize_version_mismatch, test_deserialize_bad_magic, test_deserialize_oversized_payload_field, test_deserialize_zero_payload, test_deserialize_version_zero_rejected, test_proto_version_in_wire_frame |
| REQ-3.2.8 | Wire format version and magic enforcement          | src/core/ProtocolVersion.hpp, src/core/Serializer.hpp, src/core/Serializer.cpp | test_Serializer.cpp :: test_deserialize_version_mismatch, test_deserialize_bad_magic, test_deserialize_version_zero_rejected, test_proto_version_in_wire_frame |
| REQ-3.2.4 | ACK handling                                       | src/core/AckTracker.hpp, src/core/AckTracker.cpp            | test_MessageEnvelope.cpp :: test_envelope_make_ack; test_AckTracker.cpp :: test_track_ok, test_track_full, test_on_ack_ok, test_on_ack_wrong_src, test_on_ack_wrong_id, test_sweep_expired_pending, test_sweep_pending_not_expired, test_sweep_releases_acked, test_sweep_buf_capacity, test_stats_timeout_multiround, test_ack_tracker_cancel |
| REQ-3.2.5 | Retry logic                                        | src/core/RetryManager.hpp, src/core/RetryManager.cpp        | test_RetryManager.cpp :: test_schedule_ok, test_schedule_full, test_on_ack_ok, test_on_ack_not_found, test_collect_due_immediate, test_collect_not_due, test_collect_expired, test_collect_exhausted, test_backoff_doubles, test_backoff_cap, test_collect_never_expires, test_on_ack_wrong_src_active, test_on_ack_wrong_id_active, test_collect_due_buf_cap_limits |
| REQ-3.2.6 | Duplicate suppression                              | src/core/DuplicateFilter.hpp, src/core/DuplicateFilter.cpp  | test_DuplicateFilter.cpp :: all five tests           |
| REQ-3.2.7 | Expiry handling on delivery path                   | src/core/DeliveryEngine.hpp, src/core/DeliveryEngine.cpp    | test_DeliveryEngine.cpp :: test_receive_expired, test_send_expired_returns_err, test_receive_expired_at_boundary, test_receive_zero_expiry_never_drops |
| REQ-3.3.1 | Best effort delivery semantic                      | src/core/DeliveryEngine.cpp, src/core/Types.hpp             | test_DeliveryEngine.cpp :: test_send_best_effort, test_receive_data_best_effort |
| REQ-3.3.2 | Reliable with ACK delivery semantic                | src/core/DeliveryEngine.cpp, src/core/AckTracker.cpp        | test_AckTracker.cpp :: all fourteen tests; test_DeliveryEngine.cpp :: test_send_reliable_ack, test_sweep_detects_timeout, test_send_ack_tracker_full |
| REQ-3.3.3 | Reliable with retry and dedupe semantic            | src/core/DeliveryEngine.cpp, src/core/RetryManager.cpp, src/core/DuplicateFilter.cpp | test_RetryManager.cpp :: all nineteen tests; test_DeliveryEngine.cpp :: test_send_reliable_retry, test_receive_duplicate, test_receive_ack_cancels_retry, test_pump_retries_fires, test_pump_retries_resends, test_send_retry_manager_full |
| REQ-3.3.4 | Expiring messages delivery semantic                | src/core/DeliveryEngine.cpp, src/core/Timestamp.cpp         | test_DeliveryEngine.cpp :: test_receive_expired, test_send_expired_returns_err, test_receive_expired_at_boundary, test_receive_zero_expiry_never_drops; test_Timestamp.cpp :: test_expiry_zero_never_expires |
| REQ-3.3.5 | Ordered / unordered channel support                | src/core/ChannelConfig.hpp                                  | — (not implemented: OrderingMode stored in ChannelConfig but never enforced by any transport or DeliveryEngine; no sequence-number field in MessageEnvelope; see TODO_FOR_CLASS_B_CERT.txt Item 11) |
| REQ-4.1.1 | init(config) transport operation                   | src/core/TransportInterface.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | — |
| REQ-4.1.2 | send_message(envelope) transport operation         | src/core/TransportInterface.hpp, src/core/RingBuffer.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp :: basic send/receive, bidirectional, queue-full |
| REQ-4.1.3 | receive_message(timeout) transport operation       | src/core/TransportInterface.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp :: basic send/receive, bidirectional, receive timeout |
| REQ-4.1.4 | flush / close transport operation                  | src/core/TransportInterface.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp (implicit on teardown) |
| REQ-4.2.1 | Logical channels with priority, reliability, order | src/core/ChannelConfig.hpp                                  | Priority serialised on wire (test_Serializer.cpp); reliability class fully tested; ordering not implemented (see REQ-3.3.5) |
| REQ-4.2.2 | Transport configuration (timeouts, limits)         | src/core/ChannelConfig.hpp, src/platform/TcpBackend.cpp, src/platform/TlsTcpBackend.cpp, src/core/DeliveryEngine.cpp | send_timeout_ms consumed by TcpBackend/TlsTcpBackend; recv_timeout_ms consumed by DeliveryEngine (ack_deadline); no isolated unit test |
| REQ-5.1.1 | Fixed latency impairment                           | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: fixed-latency gating   |
| REQ-5.1.2 | Random jitter impairment                           | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: test_jitter             |
| REQ-5.1.3 | Packet loss impairment                             | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: deterministic 100% loss, zero loss |
| REQ-5.1.4 | Packet duplication impairment                      | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: test_duplication, test_duplication_no_fire, test_duplication_buffer_full_skip |
| REQ-5.1.5 | Packet reordering impairment                       | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: test_process_inbound_passthrough, test_process_inbound_reorder, test_reorder_window_zero_enabled |
| REQ-5.1.6 | Partition / intermittent outage impairment         | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: test_partition_state_machine, test_partition_waiting_and_active, test_partition_gap_ms_minimum_valid |
| REQ-5.2.1 | Structured impairment configuration object        | src/platform/ImpairmentConfig.hpp, src/platform/ImpairmentConfigLoader.hpp, src/platform/ImpairmentConfigLoader.cpp | test_ImpairmentEngine.cpp :: disabled pass-through; test_ImpairmentConfigLoader.cpp :: all twelve tests |
| REQ-5.2.2 | Enable / disable each impairment independently     | src/platform/ImpairmentConfig.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: disabled pass-through  |
| REQ-5.2.3 | Per-channel / per-peer impairment config           | src/platform/ImpairmentConfig.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/DtlsUdpBackend.cpp, src/platform/LocalSimHarness.cpp | Implemented: all four backends read channels[0].impairment at init; no isolated unit test |
| REQ-5.2.4 | Deterministic PRNG mode with seed                  | src/platform/ImpairmentConfig.hpp, src/platform/PrngEngine.hpp, src/platform/PrngEngine.cpp | test_ImpairmentEngine.cpp :: deterministic 100% loss |
| REQ-5.2.5 | Snapshot / log impairment decisions                | src/platform/ImpairmentEngine.cpp                           | — (no dedicated test)                                |
| REQ-5.3.1 | Same seed → same impairment sequence               | src/platform/PrngEngine.hpp, src/platform/PrngEngine.cpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: PRNG reproducibility   |
| REQ-5.3.2 | Impairment engine usable with LocalSimHarness      | src/platform/ImpairmentEngine.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp (implicit — LocalSim uses ImpairmentEngine) |
| REQ-6.1.1 | TCP 3-way handshake establishment                  | src/platform/TcpBackend.cpp, src/platform/SocketUtils.cpp   | — (no dedicated unit test)                           |
| REQ-6.1.2 | TCP configurable connection timeouts and retry     | src/platform/TcpBackend.cpp, src/platform/SocketUtils.cpp   | — (no dedicated unit test)                           |
| REQ-6.1.3 | TCP graceful / abrupt disconnect handling          | src/platform/TcpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.1.4 | TCP application-level flow control                 | src/platform/TcpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.1.5 | TCP length-prefix message framing                  | src/platform/SocketUtils.cpp, src/platform/TcpBackend.cpp   | — (no dedicated unit test)                           |
| REQ-6.1.6 | TCP partial read / write handling                  | src/platform/SocketUtils.cpp, src/platform/TcpBackend.cpp   | — (no dedicated unit test)                           |
| REQ-6.1.7 | TCP multi-connection support                       | src/platform/TcpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.2.1 | UDP optional ACK/NAK + retry                       | src/platform/UdpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.2.2 | UDP sequence numbers and duplicate detection       | src/platform/UdpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.2.3 | UDP message size limits                            | src/platform/UdpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.2.4 | UDP source address validation                      | src/platform/UdpBackend.cpp                                 | tests/test_UdpBackend.cpp :: test_recv_wrong_source_dropped |
| REQ-6.3.1 | Configurable IP/port binding with SO_REUSEADDR     | src/platform/SocketUtils.cpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp | — |
| REQ-6.3.2 | Explicit error handling recoverable / fatal        | src/platform/SocketUtils.cpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp | — |
| REQ-6.3.3 | Read / write timeouts                              | src/platform/SocketUtils.cpp                                | test_LocalSim.cpp :: receive timeout                 |
| REQ-6.3.4 | TLS / DTLS extension point                         | src/core/TlsConfig.hpp, src/core/ChannelConfig.hpp, src/platform/TlsTcpBackend.hpp, src/platform/TlsTcpBackend.cpp, src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | tests/test_TlsTcpBackend.cpp (7 tests), tests/test_DtlsUdpBackend.cpp (6 tests) |
| REQ-6.3.5 | Bounded explicit buffer sizes                      | src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp    | — (no dedicated unit test)                           |
| REQ-6.4.1 | DTLS handshake over UDP (RFC 6347 / RFC 9147)      | src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | tests/test_DtlsUdpBackend.cpp :: test_dtls_loopback  |
| REQ-6.4.2 | DTLS cookie anti-replay exchange (server)          | src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | tests/test_DtlsUdpBackend.cpp :: test_dtls_loopback  |
| REQ-6.4.3 | DTLS retransmission timer callbacks                | src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | tests/test_DtlsUdpBackend.cpp :: test_dtls_loopback  |
| REQ-6.4.4 | DTLS MTU enforcement (DTLS_MAX_DATAGRAM_BYTES)     | src/core/Types.hpp, src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | tests/test_DtlsUdpBackend.cpp :: test_oversized_payload_rejected |
| REQ-6.4.5 | DTLS plaintext fallback (swappable without changing higher layers) | src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | tests/test_DtlsUdpBackend.cpp :: test_plaintext_loopback |
| REQ-7.1.1 | Log connection establishment and teardown          | src/core/Logger.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/DtlsUdpBackend.cpp | — (observable in output) |
| REQ-7.1.2 | Log major state changes                            | src/core/Logger.hpp                                         | — (observable in output)                             |
| REQ-7.1.3 | Log errors and FATAL with debug context            | src/core/Logger.hpp                                         | — (observable in output)                             |
| REQ-7.1.4 | No full payload logging by default                 | src/core/Logger.hpp                                         | — (convention; no automated test)                    |
| REQ-7.2.1 | Latency distribution metrics hooks                 | src/core/DeliveryStats.hpp, src/core/DeliveryEngine.hpp, src/core/DeliveryEngine.cpp | test_DeliveryEngine.cpp :: test_stats_latency        |
| REQ-7.2.2 | Loss / duplication / reordering rate metrics       | src/core/DeliveryStats.hpp, src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp, src/platform/LocalSimHarness.cpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/TlsTcpBackend.cpp, src/platform/DtlsUdpBackend.cpp | test_ImpairmentEngine.cpp :: test_stats_loss, test_stats_partition, test_stats_duplicate |
| REQ-7.2.3 | Retry / timeout / failure counters                 | src/core/DeliveryStats.hpp, src/core/AckTracker.hpp, src/core/AckTracker.cpp, src/core/RetryManager.hpp, src/core/RetryManager.cpp, src/core/DeliveryEngine.hpp, src/core/DeliveryEngine.cpp | test_AckTracker.cpp :: test_stats_timeout, test_stats_ack_received; test_RetryManager.cpp :: test_stats_retry_sent, test_stats_exhausted, test_stats_expired, test_stats_ack_received; test_DeliveryEngine.cpp :: test_stats_msgs_sent, test_stats_msgs_received, test_stats_dropped_expired, test_stats_dropped_duplicate |
| REQ-7.2.4 | Connection / restart / fatal event counters        | src/core/DeliveryStats.hpp, src/core/AssertState.hpp, src/core/AssertState.cpp, src/core/TransportInterface.hpp, src/platform/LocalSimHarness.hpp, src/platform/LocalSimHarness.cpp, src/platform/TcpBackend.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.hpp, src/platform/UdpBackend.cpp, src/platform/TlsTcpBackend.hpp, src/platform/TlsTcpBackend.cpp, src/platform/DtlsUdpBackend.hpp, src/platform/DtlsUdpBackend.cpp | — (fatal count verified indirectly via AssertState tests; connection counts exercised by transport integration tests) |

## Coverage gaps (requirements with no test)

Unimplemented features (no test expected until feature is built):
REQ-3.3.5, REQ-4.2.1 (ordering enforcement) — OrderingMode field exists in
  ChannelConfig but is never read by any transport or DeliveryEngine.
  Requires sequence numbers in MessageEnvelope and a receiver reorder buffer.
  See TODO_FOR_CLASS_B_CERT.txt Item 11.

Implemented but no isolated unit test (tested indirectly or via integration):
REQ-4.1.1, REQ-4.2.2,
REQ-5.2.5,
REQ-6.1.1 through REQ-6.1.7, REQ-6.2.1 through REQ-6.2.4,
REQ-6.3.1, REQ-6.3.2, REQ-6.3.5,
REQ-7.1.1 through REQ-7.1.4

Resolved since last generation (tests added):
- REQ-3.2.4 — now covered by test_AckTracker.cpp (10 tests)
- REQ-3.2.5 — now covered by test_RetryManager.cpp (12 tests)
- REQ-3.3.1 — now covered by test_DeliveryEngine.cpp
- REQ-3.3.2 — now covered by test_AckTracker.cpp + test_DeliveryEngine.cpp
- REQ-3.3.3 — now covered by test_RetryManager.cpp + test_DeliveryEngine.cpp
- REQ-3.2.7 — now covered by test_DeliveryEngine.cpp :: test_receive_expired, test_send_expired_returns_err, test_receive_expired_at_boundary, test_receive_zero_expiry_never_drops
- REQ-3.3.4 — now covered by test_DeliveryEngine.cpp :: test_receive_expired, test_send_expired_returns_err, test_receive_expired_at_boundary, test_receive_zero_expiry_never_drops
- REQ-5.2.1 — now also covered by test_ImpairmentConfigLoader.cpp (12 tests); ImpairmentConfigLoader.hpp/.cpp added as Implements entries
- REQ-6.3.4 — now implemented by TlsConfig.hpp, ChannelConfig.hpp, TlsTcpBackend.hpp/.cpp, DtlsUdpBackend.hpp/.cpp; verified by test_TlsTcpBackend.cpp (7 tests) and test_DtlsUdpBackend.cpp (6 tests)
- REQ-6.4.1 — now implemented by DtlsUdpBackend.hpp/.cpp; verified by test_DtlsUdpBackend.cpp :: test_dtls_loopback
- REQ-6.4.2 — now implemented by DtlsUdpBackend.hpp/.cpp (mbedtls_ssl_cookie_ctx, MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED); verified by test_DtlsUdpBackend.cpp :: test_dtls_loopback
- REQ-6.4.3 — now implemented by DtlsUdpBackend.hpp/.cpp (mbedtls_timing_delay_context); verified by test_DtlsUdpBackend.cpp :: test_dtls_loopback
- REQ-6.4.4 — now implemented by Types.hpp (DTLS_MAX_DATAGRAM_BYTES), DtlsUdpBackend.hpp/.cpp; verified by test_DtlsUdpBackend.cpp :: test_oversized_payload_rejected
- REQ-6.4.5 — now implemented by DtlsUdpBackend.hpp/.cpp (tls_enabled=false plaintext fallback); verified by test_DtlsUdpBackend.cpp :: test_plaintext_loopback
- REQ-5.1.2 — now verified by test_ImpairmentEngine.cpp :: test_jitter
- REQ-5.1.4 — now verified by test_ImpairmentEngine.cpp :: test_duplication, test_duplication_no_fire, test_duplication_buffer_full_skip
- REQ-5.1.5 — now verified by test_ImpairmentEngine.cpp :: test_process_inbound_passthrough, test_process_inbound_reorder, test_reorder_window_zero_enabled
- REQ-5.1.6 — now verified by test_ImpairmentEngine.cpp :: test_partition_state_machine, test_partition_waiting_and_active, test_partition_gap_ms_minimum_valid
- REQ-3.2.1 — now covered by test_MessageId.cpp (4 tests: seed, monotonic increment, wraparound recovery, min valid seed)
- REQ-3.2.2 — now covered by test_Timestamp.cpp (6 tests: expired false, expired at deadline, expired past deadline, zero-expiry never-expires, deadline arithmetic, zero-duration deadline)
- REQ-7.2.1 — now implemented by DeliveryStats.hpp, DeliveryEngine.hpp/.cpp (RTT tracking via AckTracker.get_send_timestamp()); verified by test_DeliveryEngine.cpp :: test_stats_latency
- REQ-7.2.2 — now implemented by ImpairmentEngine.hpp/.cpp (loss_drops, partition_drops, duplicate_injects, reorder_buffered counters); verified by test_ImpairmentEngine.cpp :: test_stats_loss, test_stats_partition, test_stats_duplicate
- REQ-7.2.3 — now implemented by AckTracker, RetryManager, DeliveryEngine (per-component stats structs in DeliveryStats.hpp); verified by test_AckTracker.cpp (2 tests), test_RetryManager.cpp (4 tests), test_DeliveryEngine.cpp (4 tests)
- REQ-7.2.4 — now implemented by AssertState.hpp/.cpp (g_fatal_count), TransportInterface + all 5 backends (connections_opened/closed); no new dedicated unit test (exercised via existing AssertState and transport tests)
