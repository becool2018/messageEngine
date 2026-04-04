# messageEngine Use Case Index

Actors: **User** (application / developer) | **System** (messageEngine — grey box)

---

## HL-1: Fire-and-Forget Message Send
> User submits a best-effort envelope; System transmits it once with no acknowledgement or retry tracking.

- UC_01 — Best-effort send over TCP (no ACK, no retry, no dedup)

---

## HL-2: Send with Acknowledgement
> User sends a reliable-ACK envelope; System transmits once and tracks the pending ACK until resolved or timed out.

- UC_02 — RELIABLE_ACK send: single transmission, ACK slot allocated in AckTracker
- UC_08 — ACK received: AckTracker slot transitions from PENDING → ACKED and is freed

---

## HL-3: Send with Automatic Retry
> User sends a reliable-retry envelope; System retransmits until ACK received, expiry reached, or retry budget exhausted, with deduplication on the receiver side.

- UC_03 — RELIABLE_RETRY send: message scheduled in RetryManager with exponential backoff
- UC_10 — RetryManager fires a scheduled retry (backoff interval elapsed)
- UC_12 — Retry cancelled on ACK receipt (RetryManager slot freed)

---

## HL-4: Send with Expiry Deadline
> User sets expiry_time_us on an envelope; System drops the message on the send path if the deadline has passed.

- UC_04 — Expired message detected and dropped at send time

---

## HL-5: Receive a Message
> User calls receive_message(); System returns the next available envelope, applying deduplication and expiry filtering.

- UC_45 — Receive happy path: DATA envelope delivered successfully (dedup passes, not expired)
- UC_09 — Incoming message dropped at receive because expiry_time_us has passed

---

## HL-6: Duplicate Message Suppression
> System silently drops any message whose (source_id, message_id) pair has already been seen within the sliding dedup window.

- UC_07 — Duplicate detected and dropped by DuplicateFilter::check_and_record()
- UC_33 — Sliding-window eviction: oldest entry evicted when DEDUP_WINDOW_SIZE entries are full

---

## HL-7: Start a Server Endpoint
> User initialises a transport in server mode; System binds to the configured IP/port and begins accepting connections.

- UC_19 — TCP server bind, listen, and non-blocking accept loop
- UC_35 — (See HL-8) Client connects; server accept fd becomes a client slot

---

## HL-8: Start a Client Endpoint
> User initialises a transport in client mode; System connects to the configured peer IP/port within the configured timeout.

- UC_35 — TCP client connect with configurable timeout

---

## HL-9: Close a Transport
> User calls close(); System flushes any pending delayed messages and releases all socket file descriptors and TLS contexts.

- UC_34 — Transport teardown: flush impairment delay buffer, close all fds, reset state

---

## HL-10: Pump the Retry Loop
> User calls DeliveryEngine::pump_retries() in the application event loop; System collects all RetryManager entries whose backoff interval has elapsed and retransmits them.

- UC_10 — Scheduled retry fires: message retransmitted; backoff interval doubled for next attempt
- UC_12 — Retry entry removed when corresponding ACK is received

---

## HL-11: Sweep ACK Timeouts
> User calls DeliveryEngine::sweep_ack_timeouts() in the application event loop; System identifies AckTracker entries whose deadline has passed and returns them as unacknowledged.

- UC_11 — AckTracker sweep: PENDING entries past deadline_us collected as expired

---

## HL-12: Configure Network Impairments
> User provides an ImpairmentConfig with one or more impairments enabled; System applies them to outbound and/or inbound message flows.

- UC_13 — Packet loss: outbound message dropped with configured probability
- UC_14 — Packet duplication: additional copy queued for delayed delivery
- UC_15 — Fixed latency: message held in delay buffer until release_us elapses
- UC_16 — Jitter: per-message delay drawn from uniform distribution around mean
- UC_17 — Reordering: inbound messages buffered and released out of arrival order
- UC_18 — Partition: all traffic blocked for partition_duration_ms, then released for partition_gap_ms
- UC_32 — 100% loss configuration: every outbound message dropped, none delivered

---

## HL-13: Deterministic Impairment Replay
> User seeds the PrngEngine with a fixed value; System produces the identical sequence of loss/jitter/duplication decisions for any given input stream.

- UC_29 — PRNG seeded with known value before test run
- UC_31 — Two runs with identical seed and message stream produce identical impairment outcomes

---

## HL-14: In-Process Simulation
> User creates two LocalSimHarness instances and links them; System delivers messages between them in-process without any real sockets, respecting all configured impairments.

- UC_24 — LocalSimHarness: send on one harness, receive on linked peer
- UC_30 — LocalSimHarness round-trip: message sent, impaired, and received in one process
- UC_42 — LocalSimHarness inject(): test code directly injects an envelope without going through send_message()

---

## HL-15: Observability (Logging)
> System emits structured log entries at INFO / WARNING_LO / WARNING_HI / FATAL severity for all connection events, state changes, and errors; User reads log output to diagnose issues.

- UC_27 — Configuration defaults applied and logged at init time
- UC_46 — Logger::log() called directly by User application code to record application-level events

---

## HL-16: System Initialization
> User constructs a DeliveryEngine, wires it to a TransportInterface, and calls init(); System allocates fixed internal state and validates configuration.

- UC_47 — transport_config_default() and channel_config_default(): User fills TransportConfig and ChannelConfig with safe defaults before customising
- UC_27 — TransportConfig defaults and per-channel ChannelConfig overrides applied
- UC_28 — DeliveryEngine::init(): DuplicateFilter, AckTracker, RetryManager all reset

---

## HL-17: UDP Plaintext Transport
> User initialises a UdpBackend; System sends and receives discrete UDP datagrams with no connection management, applying any configured impairments.

- UC_22 — UDP send: envelope serialised, impairments applied, datagram sent via sendto()
- UC_23 — UDP receive: datagram received via recvfrom(), deserialised, enqueued for delivery

---

## HL-18: TLS-Encrypted TCP Transport
> User initialises a TlsTcpBackend with a TlsConfig (cert/key/CA paths, verify_peer); System performs a TLS handshake on each connection before sending or receiving framed envelopes. When tls_enabled == false the backend falls back to plaintext TCP, allowing the same code path in test mode.

- UC_36 — TLS server: bind, listen, accept, and complete TLS handshake with each client
- UC_37 — TLS client: connect to peer and complete TLS handshake before first message

---

## HL-19: DTLS-Encrypted UDP Transport
> User initialises a DtlsUdpBackend with a TlsConfig; System performs a DTLS handshake (including cookie exchange on the server) before sending and receiving encrypted datagrams. Oversized messages are rejected before encryption. When tls_enabled == false, plaintext UDP is used without any handshake.

- UC_38 — DTLS server: bind, receive ClientHello, perform cookie exchange, complete DTLS handshake
- UC_39 — DTLS client: connect UDP socket to peer, complete DTLS handshake
- UC_40 — MTU enforcement: outbound message whose serialised size exceeds DTLS_MAX_DATAGRAM_BYTES is rejected with ERR_INVALID before any encryption attempt

---

## HL-20: Load Impairment Config from File
> User provides a path to a key=value text file; System parses it into an ImpairmentConfig, clamping probability values to [0.0, 1.0], and returns ERR_IO on file-open failure.

- UC_41 — impairment_config_load(): read file, parse all recognised keys, clamp probabilities, return populated ImpairmentConfig

---

## HL-21: Build and Classify a Message Envelope
> User calls envelope helper functions to zero-initialise an envelope before populating fields, and to classify a received envelope as data or control before processing it.

- UC_43 — envelope_init(): zero-initialise a MessageEnvelope to a safe, INVALID state before field assignment
- UC_44 — envelope_is_data() / envelope_is_control() / envelope_valid(): classify or validate an envelope type and contents

---

## HL-22: Read Monotonic Time and Compute Deadlines
> User calls timestamp utilities to read the current monotonic clock and compute absolute deadline timestamps for message expiry and ACK timeout tracking.

- UC_48 — timestamp_now_us(): read CLOCK_MONOTONIC in microseconds for use in send, receive, pump, and sweep calls
- UC_49 — timestamp_deadline_us(): compute an absolute expiry deadline from current time and a duration in milliseconds

---

## HL-23: Poll DeliveryEngine Observability Events
> User calls DeliveryEngine::poll_event() in the application event loop; System returns the next unread delivery event from the bounded DeliveryEventRing (send success/failure, ACK received, retry fired, ACK timeout, duplicate drop, expiry drop, misroute drop).

- UC_59 — poll_event(): dequeue one DeliveryEvent from the bounded ring; returns ERR_EMPTY when no events are pending
- UC_60 — pending_event_count(): return the number of unread events currently held in the DeliveryEventRing

---

## HL-24: Inbound Impairment Applied on UDP/DTLS Receive
> System calls ImpairmentEngine::process_inbound() on every datagram received by UdpBackend or DtlsUdpBackend, applying partition drops and reordering before pushing the envelope to the inbound ring.

- UC_53 — ImpairmentEngine process_outbound/collect_deliverable/process_inbound — called internally by each backend on every send/receive; never by the user (updated: process_inbound now active in UdpBackend and DtlsUdpBackend)

---

## HL-26: Inbound Impairment on TCP/TLS Receive Path
> System calls ImpairmentEngine::process_inbound() on every framed message received by TcpBackend or TlsTcpBackend, applying partition drops and reordering before pushing the envelope to the inbound ring. Blocked messages are dropped with ERR_IO or held in the delay buffer.

- UC_53 — ImpairmentEngine process_inbound — called internally by TcpBackend and TlsTcpBackend on every received frame (updated: phase 2 wiring active in both TCP backends)

---

## HL-27: LocalSim Inbound Impairment via deliver_from_peer()
> System applies ImpairmentEngine::process_inbound() on all linked-peer deliveries via LocalSimHarness::deliver_from_peer(); inject() remains a raw test-hook that bypasses impairment by contract, preserving direct test access to the inbound ring without any impairment filtering.

- UC_24 — LocalSimHarness: send on one harness, receive on linked peer (updated: deliver_from_peer() now applies process_inbound() before enqueue)
- UC_42 — LocalSimHarness inject(): test code directly injects an envelope without going through send_message() or deliver_from_peer() — raw bypass; no impairment applied

---

## HL-25: TLS Session Resumption on Reconnect
> User enables TLS session resumption via TlsConfig::session_resumption_enabled; System saves the session after the first handshake and presents it on reconnect to skip the full handshake exchange, reducing reconnection latency (RFC 5077 / TLS 1.3 PSK).

- UC_37 — TLS client: connect to peer and complete TLS handshake before first message (updated: session ticket saved/presented when session_resumption_enabled is true)
- UC_36 — TLS server: bind, listen, accept, and complete TLS handshake with each client (updated: session tickets enabled via mbedtls_ssl_conf_session_tickets when session_resumption_enabled is true)

---

## Application Workflow (above system boundary)

These use cases document patterns that combine multiple system calls and sit at
the application layer rather than at the system boundary. They are not single
User → System interactions.

- UC_05 — Server echo reply — calls receive_message() then send_message() in sequence; the two-step pattern is above the HL boundary

---

## System Internals (sub-functions, not user-facing goals)

These use cases document mechanisms that are invoked internally by the System on
behalf of other use cases. The User never calls them directly; they are invisible
at the User → System boundary.

- UC_06 — TCP inbound deserialisation — recv_from_client() called internally by TcpBackend::receive_message(); never by the user
- UC_20 — TCP send framed message — send_frame() / send_to_all_clients() called internally by TcpBackend::send_message()
- UC_21 — TCP poll and receive — poll_clients_once() called internally by TcpBackend::receive_message()
- UC_25 — Serializer encode — Serializer::serialize() called internally by all backends; never directly by the user
- UC_26 — Serializer decode — Serializer::deserialize() called internally by all backends; never directly by the user
- UC_50 — envelope_copy() — copies envelope bytes between internal buffers; called by RingBuffer::push/pop and DeliveryEngine; never by the user
- UC_51 — envelope_make_ack() — constructs an ACK reply envelope; called internally by DeliveryEngine::receive() for RELIABLE_ACK/RETRY messages; never by the user
- UC_52 — RingBuffer push/pop — SPSC lock-free queue used internally by all backends to stage inbound envelopes between recv threads and receive_message(); never called by the user
- UC_53 — ImpairmentEngine process_outbound/collect_deliverable/process_inbound — called internally by each backend on every send/receive; never by the user
- UC_54 — PrngEngine next/next_double/next_range — xorshift64 PRNG called internally by ImpairmentEngine to make loss/jitter/duplication decisions; never by the user
- UC_55 — MessageIdGen next() — monotonic message-ID counter owned by DeliveryEngine; assigns unique IDs on outbound send; never called by the user
- UC_56 — AssertState / IResetHandler / AbortResetHandler — assertion-failure flag and handler dispatch; infrastructure for NEVER_COMPILED_OUT_ASSERT; not called from application code
- UC_57 — ISocketOps / SocketOpsImpl — injectable POSIX socket adapter used by TcpBackend, UdpBackend, TlsTcpBackend, DtlsUdpBackend for fault injection in tests; production code uses the singleton SocketOpsImpl; never called by the user
- UC_58 — IMbedtlsOps / MbedtlsOpsImpl — injectable mbedTLS adapter used by DtlsUdpBackend for fault injection in tests; production code uses the singleton MbedtlsOpsImpl; never called by the user
