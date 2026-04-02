# Use Case Frequency Classification

This document classifies all 58 use cases by how often they execute during
normal application operation. Use this as a guide for performance analysis,
code review prioritisation, and deciding where to focus profiling effort.

---

## Hottest Path — called on every message and every event loop tick

These use cases execute on every send, every receive, or every event loop
iteration regardless of message volume. Correctness and performance here
directly impact all traffic.

| UC | Description | Why so frequent |
|----|-------------|-----------------|
| **UC_45** | Receive happy path: DATA envelope delivered successfully | Called every time the application polls for messages |
| **UC_01** | Best-effort send over TCP (no ACK, no retry, no dedup) | Most common send mode; executes on every outbound message |
| **UC_25** | Serializer::serialize() — encode envelope to wire bytes | Invoked on every outbound message before transmission |
| **UC_26** | Serializer::deserialize() — decode wire bytes to envelope | Invoked on every inbound message after reception |
| **UC_52** | RingBuffer push/pop — SPSC lock-free inbound staging queue | Every send and receive passes through the ring buffer |
| **UC_10** | RetryManager fires a scheduled retry (backoff interval elapsed) | Executed inside every `DeliveryEngine::pump_retries()` tick |
| **UC_11** | AckTracker sweep: PENDING entries past deadline collected | Executed inside every `DeliveryEngine::sweep_ack_timeouts()` tick |

---

## High Frequency — called frequently during active messaging

These use cases fire on a per-message or per-event basis whenever reliability
modes, impairments, or error conditions are active.

| UC | Description | When triggered |
|----|-------------|----------------|
| **UC_02** | RELIABLE_ACK send: single transmission, ACK slot allocated | Every message sent with `RELIABLE_ACK` reliability class |
| **UC_03** | RELIABLE_RETRY send: message scheduled in RetryManager | Every message sent with `RELIABLE_RETRY` reliability class |
| **UC_08** | ACK received: AckTracker slot transitions PENDING → ACKED | One per successfully acknowledged message |
| **UC_12** | Retry cancelled on ACK receipt: RetryManager slot freed | One per ACK when a retry entry is outstanding |
| **UC_07** | Duplicate detected and dropped by DuplicateFilter | Frequent under retry or network-partition conditions |
| **UC_53** | ImpairmentEngine process_outbound / collect_deliverable / process_inbound | Every send and receive when any impairment is configured |
| **UC_54** | PrngEngine next() / next_double() / next_range() | Every loss, jitter, or duplication decision in ImpairmentEngine |

---

## Medium Frequency — per-connection or on error / expiry conditions

These use cases fire less often — on connection events, message expiry, or
when impairments are actively simulating faults.

| UC | Description | When triggered |
|----|-------------|----------------|
| **UC_04** | Expired message detected and dropped at send time | When `expiry_time_us` is set and the deadline has passed |
| **UC_09** | Incoming message dropped at receive: expiry_time_us has passed | Stale messages that arrive after their deadline |
| **UC_13** | Packet loss: outbound message dropped with configured probability | Every outbound message when loss impairment is enabled |
| **UC_14** | Packet duplication: additional copy queued for delayed delivery | Every outbound message when duplication impairment is enabled |
| **UC_15** | Fixed latency: message held in delay buffer until release_us elapses | Every outbound message when fixed-latency impairment is enabled |
| **UC_16** | Jitter: per-message delay drawn from uniform distribution | Every outbound message when jitter impairment is enabled |
| **UC_17** | Reordering: inbound messages buffered and released out of order | Every inbound message when reordering impairment is enabled |
| **UC_18** | Partition: all traffic blocked, then released | Every message while a partition interval is active |
| **UC_20** | TCP send framed message — send_frame() / send_to_all_clients() | Every TCP outbound message (internal to TcpBackend) |
| **UC_21** | TCP poll and receive — poll_clients_once() | Every TCP receive poll (internal to TcpBackend) |
| **UC_22** | UDP send: envelope serialised, impairments applied, datagram sent | Every UDP outbound message |
| **UC_23** | UDP receive: datagram received, deserialised, enqueued | Every UDP inbound message |
| **UC_06** | TCP inbound deserialisation — recv_from_client() | Every TCP inbound message (internal to TcpBackend) |

---

## Low Frequency — session lifecycle, startup, and teardown

These use cases fire once per session, once per connection, or only during
testing and configuration. Correctness matters; performance is not the concern.

| UC | Description | When triggered |
|----|-------------|----------------|
| UC_19 | TCP server bind, listen, and non-blocking accept loop | Once at server startup |
| UC_35 | TCP client connect with configurable timeout | Once per outbound connection |
| UC_36 | TLS server: bind, listen, accept, complete TLS handshake | Once per TLS server connection |
| UC_37 | TLS client: connect to peer, complete TLS handshake | Once per TLS client connection |
| UC_38 | DTLS server: bind, cookie exchange, complete DTLS handshake | Once per DTLS server session |
| UC_39 | DTLS client: connect UDP socket, complete DTLS handshake | Once per DTLS client session |
| UC_28 | DeliveryEngine::init(): all subsystems reset | Once at application startup |
| UC_34 | Transport teardown: flush delay buffer, close all fds | Once at shutdown |
| UC_27 | TransportConfig defaults and ChannelConfig overrides applied | Once at init |
| UC_47 | transport_config_default() and channel_config_default() | Once at init before customisation |
| UC_41 | impairment_config_load(): read file, parse keys, clamp probabilities | Once at startup when loading from file |
| UC_24 | LocalSimHarness: send on one harness, receive on linked peer | Per message in in-process simulation tests |
| UC_30 | LocalSimHarness round-trip: message sent, impaired, received | Per test scenario in simulation |
| UC_42 | LocalSimHarness inject(): direct envelope injection without send_message() | Per injection call in test code |
| UC_29 | PRNG seeded with known value before test run | Once per deterministic test run |
| UC_31 | Two runs with identical seed produce identical impairment outcomes | Once per reproducibility verification |
| UC_32 | 100% loss configuration: every outbound message dropped | Per test scenario validating total loss |
| UC_33 | Sliding-window eviction: oldest entry evicted when DEDUP_WINDOW_SIZE full | When dedup window reaches capacity |
| UC_40 | MTU enforcement: oversized DTLS message rejected before encryption | Per oversized message attempt |
| UC_43 | envelope_init(): zero-initialise a MessageEnvelope | Once per envelope before field assignment |
| UC_44 | envelope_is_data() / envelope_is_control() / envelope_valid() | Per envelope classification call |
| UC_45 | Receive happy path (also in hottest path — listed here for completeness) | — |
| UC_46 | Logger::log() called directly by user application code | On application-defined log events |
| UC_48 | timestamp_now_us(): read CLOCK_MONOTONIC in microseconds | Every send, receive, pump, and sweep call |
| UC_49 | timestamp_deadline_us(): compute absolute expiry deadline | Every time an expiry or ACK timeout is configured |

---

## System Internal sub-functions (frequency inherited from caller)

These use cases are never called directly by the application; their execution
frequency is determined entirely by whichever higher-level UC invokes them.

| UC | Description | Inherits frequency from |
|----|-------------|------------------------|
| UC_50 | envelope_copy() — copy envelope bytes between internal buffers | UC_52 (RingBuffer) and DeliveryEngine send/receive — hottest path |
| UC_51 | envelope_make_ack() — construct ACK reply envelope | UC_08 (ACK received) — high frequency |
| UC_55 | MessageIdGen next() — assign unique message ID on outbound send | UC_01 / UC_02 / UC_03 — hottest path |
| UC_56 | AssertState / IResetHandler / AbortResetHandler | Triggered only on assertion failure — rare |
| UC_57 | ISocketOps / SocketOpsImpl — injectable POSIX socket adapter | Inherits from all TCP/UDP send/receive UCs |
| UC_58 | IMbedtlsOps / MbedtlsOpsImpl — injectable mbedTLS adapter | Inherits from UC_38 / UC_39 (DTLS) |

---

## Summary

The tightest inner loop for an application under message load is:

```
Send:    UC_01/02/03 → UC_55 → UC_25 → UC_53 → UC_52 → UC_20/22
Receive: UC_45       → UC_52 → UC_26 → UC_07  → UC_09
Tick:    UC_10  (pump_retries)
         UC_11  (sweep_ack_timeouts)
```

These seven user-facing use cases (UC_01, UC_45, UC_25, UC_26, UC_52, UC_10,
UC_11) plus their three System Internal helpers (UC_50, UC_55, UC_53) form the
performance-critical core. All other use cases are lifecycle, configuration,
simulation, or error-path operations.
