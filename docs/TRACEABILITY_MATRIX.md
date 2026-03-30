# messageEngine Traceability Matrix

Generated from `// Implements:` and `// Verifies:` comments in the codebase.
Regenerate by running: `docs/check_traceability.sh`
Policy: CLAUDE.md §11 / .claude/CLAUDE.md §10

| REQ ID    | Requirement (summary)                              | Implements (src/ file)                                      | Verifies (tests/ file :: test name)                  |
|-----------|----------------------------------------------------|-------------------------------------------------------------|------------------------------------------------------|
| REQ-3.1   | MessageEnvelope standard fields                    | src/core/MessageEnvelope.hpp, src/core/Types.hpp            | test_MessageEnvelope.cpp :: test_envelope_init, test_envelope_valid, test_envelope_copy, test_envelope_is_data |
| REQ-3.2.1 | Message ID generation and comparison               | src/core/MessageId.hpp, src/core/MessageId.cpp              | — (no dedicated test; covered indirectly by send-path tests) |
| REQ-3.2.2 | Timestamps and expiry checking                     | src/core/Timestamp.hpp, src/core/Timestamp.cpp              | — (no dedicated test)                                |
| REQ-3.2.3 | Serialization / deserialization                    | src/core/Serializer.hpp, src/core/Serializer.cpp            | test_Serializer.cpp :: all six tests                 |
| REQ-3.2.4 | ACK handling                                       | src/core/AckTracker.hpp, src/core/AckTracker.cpp            | test_MessageEnvelope.cpp :: test_envelope_make_ack; test_AckTracker.cpp :: test_track_ok, test_track_full, test_on_ack_ok, test_on_ack_wrong_src, test_on_ack_wrong_id, test_sweep_expired_pending, test_sweep_pending_not_expired, test_sweep_releases_acked, test_sweep_buf_capacity |
| REQ-3.2.5 | Retry logic                                        | src/core/RetryManager.hpp, src/core/RetryManager.cpp        | test_RetryManager.cpp :: test_schedule_ok, test_schedule_full, test_on_ack_ok, test_on_ack_not_found, test_collect_due_immediate, test_collect_not_due, test_collect_expired, test_collect_exhausted, test_backoff_doubles, test_backoff_cap, test_collect_never_expires |
| REQ-3.2.6 | Duplicate suppression                              | src/core/DuplicateFilter.hpp, src/core/DuplicateFilter.cpp  | test_DuplicateFilter.cpp :: all five tests           |
| REQ-3.2.7 | Expiry handling on delivery path                   | src/core/DeliveryEngine.hpp, src/core/DeliveryEngine.cpp    | — (no dedicated test)                                |
| REQ-3.3.1 | Best effort delivery semantic                      | src/core/DeliveryEngine.cpp, src/core/Types.hpp             | test_DeliveryEngine.cpp :: test_send_best_effort, test_receive_data_best_effort |
| REQ-3.3.2 | Reliable with ACK delivery semantic                | src/core/DeliveryEngine.cpp, src/core/AckTracker.cpp        | test_AckTracker.cpp :: all ten tests; test_DeliveryEngine.cpp :: test_send_reliable_ack, test_sweep_detects_timeout, test_send_ack_tracker_full |
| REQ-3.3.3 | Reliable with retry and dedupe semantic            | src/core/DeliveryEngine.cpp, src/core/RetryManager.cpp, src/core/DuplicateFilter.cpp | test_RetryManager.cpp :: all twelve tests; test_DeliveryEngine.cpp :: test_send_reliable_retry, test_receive_duplicate, test_receive_ack_cancels_retry, test_pump_retries_fires, test_pump_retries_resends, test_send_retry_manager_full |
| REQ-3.3.4 | Expiring messages delivery semantic                | src/core/DeliveryEngine.cpp, src/core/Timestamp.cpp         | test_DeliveryEngine.cpp :: test_receive_expired |
| REQ-3.3.5 | Ordered / unordered channel support                | src/core/ChannelConfig.hpp                                  | — (no dedicated test)                                |
| REQ-4.1.1 | init(config) transport operation                   | src/core/TransportInterface.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | — |
| REQ-4.1.2 | send_message(envelope) transport operation         | src/core/TransportInterface.hpp, src/core/RingBuffer.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp :: basic send/receive, bidirectional, queue-full |
| REQ-4.1.3 | receive_message(timeout) transport operation       | src/core/TransportInterface.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp :: basic send/receive, bidirectional, receive timeout |
| REQ-4.1.4 | flush / close transport operation                  | src/core/TransportInterface.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp, src/platform/LocalSimHarness.cpp | test_LocalSim.cpp (implicit on teardown) |
| REQ-4.2.1 | Logical channels with priority, reliability, order | src/core/ChannelConfig.hpp                                  | — (no dedicated test)                                |
| REQ-4.2.2 | Transport configuration (timeouts, limits)         | src/core/ChannelConfig.hpp                                  | — (no dedicated test)                                |
| REQ-5.1.1 | Fixed latency impairment                           | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: fixed-latency gating   |
| REQ-5.1.2 | Random jitter impairment                           | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | — (no dedicated test)                               |
| REQ-5.1.3 | Packet loss impairment                             | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: deterministic 100% loss, zero loss |
| REQ-5.1.4 | Packet duplication impairment                      | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | — (no dedicated test)                               |
| REQ-5.1.5 | Packet reordering impairment                       | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | — (no dedicated test)                               |
| REQ-5.1.6 | Partition / intermittent outage impairment         | src/platform/ImpairmentEngine.hpp, src/platform/ImpairmentEngine.cpp | — (no dedicated test)                               |
| REQ-5.2.1 | Structured impairment configuration object        | src/platform/ImpairmentConfig.hpp                           | test_ImpairmentEngine.cpp :: disabled pass-through   |
| REQ-5.2.2 | Enable / disable each impairment independently     | src/platform/ImpairmentConfig.hpp, src/platform/ImpairmentEngine.cpp | test_ImpairmentEngine.cpp :: disabled pass-through  |
| REQ-5.2.3 | Per-channel / per-peer impairment config           | src/platform/ImpairmentConfig.hpp                           | — (no dedicated test)                                |
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
| REQ-6.2.4 | UDP source address validation                      | src/platform/UdpBackend.cpp                                 | — (no dedicated unit test)                           |
| REQ-6.3.1 | Configurable IP/port binding with SO_REUSEADDR     | src/platform/SocketUtils.cpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp | — |
| REQ-6.3.2 | Explicit error handling recoverable / fatal        | src/platform/SocketUtils.cpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp | — |
| REQ-6.3.3 | Read / write timeouts                              | src/platform/SocketUtils.cpp                                | test_LocalSim.cpp :: receive timeout                 |
| REQ-6.3.4 | TLS / DTLS extension point                         | src/platform/TcpBackend.hpp, src/platform/UdpBackend.hpp    | — (not yet implemented)                              |
| REQ-6.3.5 | Bounded explicit buffer sizes                      | src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp    | — (no dedicated unit test)                           |
| REQ-7.1.1 | Log connection establishment and teardown          | src/core/Logger.hpp, src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp | — (observable in output) |
| REQ-7.1.2 | Log major state changes                            | src/core/Logger.hpp                                         | — (observable in output)                             |
| REQ-7.1.3 | Log errors and FATAL with debug context            | src/core/Logger.hpp                                         | — (observable in output)                             |
| REQ-7.1.4 | No full payload logging by default                 | src/core/Logger.hpp                                         | — (convention; no automated test)                    |
| REQ-7.2.1 | Latency distribution metrics hooks                 | src/platform/ImpairmentEngine.cpp                           | — (TODO: not fully implemented)                      |
| REQ-7.2.2 | Loss / duplication / reordering rate metrics       | src/platform/ImpairmentEngine.cpp                           | — (TODO: not fully implemented)                      |
| REQ-7.2.3 | Retry / timeout / failure counters                 | src/core/DeliveryEngine.cpp, src/core/RetryManager.cpp      | — (TODO: not fully implemented)                      |
| REQ-7.2.4 | Connection / restart / fatal event counters        | src/platform/TcpBackend.cpp, src/platform/UdpBackend.cpp    | — (TODO: not fully implemented)                      |

## Coverage gaps (requirements with no test)

The following REQ IDs have no `Verifies:` entry. These are candidates for new tests:
REQ-3.2.1, REQ-3.2.2, REQ-3.2.7,
REQ-3.3.5,
REQ-4.1.1, REQ-4.2.1, REQ-4.2.2,
REQ-5.1.2, REQ-5.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.2.3, REQ-5.2.5,
REQ-6.1.1 through REQ-6.1.7, REQ-6.2.1 through REQ-6.2.4,
REQ-6.3.1, REQ-6.3.2, REQ-6.3.4, REQ-6.3.5,
REQ-7.1.1 through REQ-7.2.4

Resolved since last generation (tests added):
- REQ-3.2.4 — now covered by test_AckTracker.cpp (10 tests)
- REQ-3.2.5 — now covered by test_RetryManager.cpp (12 tests)
- REQ-3.3.1 — now covered by test_DeliveryEngine.cpp
- REQ-3.3.2 — now covered by test_AckTracker.cpp + test_DeliveryEngine.cpp
- REQ-3.3.3 — now covered by test_RetryManager.cpp + test_DeliveryEngine.cpp
- REQ-3.3.4 — now covered by test_DeliveryEngine.cpp :: test_receive_expired
