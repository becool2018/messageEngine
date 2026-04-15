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
> User initialises a transport in server mode; System binds to the configured IP/port and begins accepting connections. After init, the server stores its local NodeId so inbound HELLO frames can be matched against the per-client routing table (HL-30).

- UC_19 — TCP server bind, listen, and non-blocking accept loop
- UC_35 — (See HL-8) Client connects; server accept fd becomes a client slot

---

## HL-8: Start a Client Endpoint
> User initialises a transport in client mode; System connects to the configured peer IP/port within the configured timeout. After the connection succeeds, DeliveryEngine calls `transport->register_local_id(local_id)` which causes the client to send a HELLO frame to the server (HL-30).

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
- UC_46 — Caller emits a structured log line via LOG_* macros; Logger routes through injected ILogSink (e.g., StderrLogSink) using timestamps from injected ILogClock (e.g., PosixLogClock)
- UC_77 — Logger::init(): User injects ILogClock and ILogSink implementations before first log call; System stores the pointers and captures PID

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

- UC_61 — poll_event() / pending_event_count(): dequeue one DeliveryEvent or query the count of unread events in the bounded ring; see HL-29 for bulk drain_events()

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

## HL-28: Bounded Request/Response via RequestReplyEngine
> User creates a RequestReplyEngine on top of a DeliveryEngine, sends a request via send_request(), and polls for a correlated response via receive_response(). Correlation metadata travels as a 12-byte RRHeader prefix inside the existing payload field; MessageEnvelope, Serializer, and PROTO_VERSION are untouched. Pending request table is bounded (MAX_PENDING_REQUESTS=16).

- UC_59 — send_request(): build RRHeader + application payload, send via RELIABLE_RETRY, record pending slot with correlation_id and expiry
- UC_60 — receive_response(): drain inbound envelopes, match by correlation_id, return application payload and free the pending slot

---

## HL-29: Bulk Observability Event Drain via drain_events()
> User calls DeliveryEngine::drain_events() to atomically pull all pending observability events into a caller-supplied buffer in a single call. Complements poll_event() for batch processing. Events are returned in FIFO order; the call is bounded by the caller-supplied buffer capacity and the ring capacity.

- UC_61 — drain_events(): drain up to buf_cap DeliveryEvents from the ring in FIFO order; returns count written; non-blocking; never modifies delivery state

---

## HL-25: TLS Session Resumption on Reconnect
> User enables TLS session resumption via TlsConfig::session_resumption_enabled; System saves the session after the first handshake and presents it on reconnect to skip the full handshake exchange, reducing reconnection latency (RFC 5077 / TLS 1.3 PSK).

- UC_37 — TLS client: connect to peer and complete TLS handshake before first message (updated: session ticket saved/presented when session_resumption_enabled is true)
- UC_36 — TLS server: bind, listen, accept, and complete TLS handshake with each client (updated: session tickets enabled via mbedtls_ssl_conf_session_tickets when session_resumption_enabled is true)

---

## HL-30: TCP/TLS Client NodeId Registration on Connect
> When a TCP or TLS client endpoint initialises, the system automatically sends a
> HELLO frame carrying the client's local NodeId to the server, enabling the server
> to build its per-client routing table.

- UC_62 — TCP client sends HELLO frame on connect
- UC_63 — TLS client sends HELLO frame on connect (via TLS session)
- UC_64 — TCP server receives HELLO and registers client NodeId
- UC_65 — TLS server receives HELLO and registers client NodeId

---

## HL-31: TCP/TLS Server Unicast Message Routing
> When a message is sent on a server endpoint with a specific destination_id, the
> server routes it only to the client whose registered NodeId matches. Broadcast
> (destination_id == 0) fans out to all clients as before.

- UC_66 — TCP server routes unicast frame to matched client slot
- UC_67 — TLS server routes unicast frame to matched TLS session
- UC_68 — Unicast send to unregistered NodeId returns ERR_INVALID

---

## HL-32: Large Message Fragmentation and Reassembly
> User sends a message whose payload exceeds FRAG_MAX_PAYLOAD_BYTES; System transparently
> splits it into up to FRAG_MAX_COUNT wire fragments on the send path, and reassembles
> them back into the original logical message on the receive path before any delivery
> logic sees it.

- UC_69 — send_fragments: DeliveryEngine detects payload > FRAG_MAX_PAYLOAD_BYTES, calls fragment_message(), transmits each wire fragment individually
- UC_70 — ReassemblyBuffer::ingest(): receive path collects wire fragments by (source_id, message_id) key, assembles them into a logical envelope on last-fragment arrival

---

## HL-33: Ordered Message Delivery (Sequence Gate)
> User sends DATA messages with sequence_num > 0 (ORDERED channel); System buffers any
> out-of-order arrivals in a bounded hold array and delivers them to the application
> strictly in sequence order. Control messages and sequence_num == 0 (UNORDERED) bypass
> the gate immediately.

- UC_71 — OrderingBuffer::ingest() + try_release_next(): in-order messages delivered immediately; out-of-order messages held; gap-filling arrivals release the backlog
- UC_74 — OrderingBuffer::sweep_expired_holds(): proactively evict expired held messages on every data-receive call to prevent permanent ordering stall on a lost gap (System Internal)

---

## HL-34: Peer Reconnect and Ordering State Reset
> When a TCP or TLS peer reconnects (a HELLO frame arrives on an already-registered
> channel), System clears the stale per-peer sequence state in the ordering gate before
> the next message is delivered, so the new connection's messages (starting at seq=1)
> are not stalled or discarded due to the prior session's next_expected_seq value.

- UC_72 — drain_hello_reconnects() / reset_peer_ordering() / OrderingBuffer::reset_peer(): ordering state and hold buffer cleared on peer reconnect (REQ-3.3.6)

---

## HL-35: Stale Reassembly Slot Reclamation
> System periodically reclaims ReassemblyBuffer slots that have been open longer than
> recv_timeout_ms without completing, preventing slot exhaustion from peers that crash
> after sending only the first fragment of a large message.

- UC_73 — ReassemblyBuffer::sweep_stale(): frees slots open longer than stale_threshold_us; called from DeliveryEngine::sweep_ack_timeouts() (REQ-3.2.9)

---

## HL-36: Injectable Logger Initialization
> Before any log output is produced, User calls Logger::init() once, supplying concrete
> ILogClock and ILogSink implementations. System stores the pointers and uses them for
> all subsequent LOG_* macro invocations. Test code injects MockLogClock / VectorLogSink
> to capture and assert on log output without writing to stderr.

- UC_77 — Logger::init(): User injects ILogClock and ILogSink implementations before first log call; System stores the pointers and captures PID

---

## HL-37: TLS=0 Optional Build (MESSAGEENGINE_NO_TLS)
> User builds messageEngine with TLS=0 (injects -DMESSAGEENGINE_NO_TLS) to produce a
> binary with no mbedTLS dependency. The TlsTcpBackend, DtlsUdpBackend, TlsSessionStore,
> and MbedtlsOpsImpl translation units are excluded from compilation. tls_demo and
> dtls_demo print an error and exit when invoked. All plaintext transports (TcpBackend,
> UdpBackend, LocalSimHarness) and all non-TLS tests remain fully operational.

- UC_78 — Build with TLS=0: User passes TLS=0 to make; System compiles without mbedTLS; TLS/DTLS backends and demos are excluded; all other functionality is unaffected

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
- UC_62 — TCP client sends HELLO frame on connect — `TcpBackend::register_local_id()` / `send_hello_frame()` called internally by DeliveryEngine after init; never by the user
- UC_63 — TLS client sends HELLO frame on connect — `TlsTcpBackend::register_local_id()` / `send_hello_frame()` called internally by DeliveryEngine after init; never by the user
- UC_64 — TCP server receives HELLO and registers client NodeId — `TcpBackend::handle_hello_frame()` called internally by `recv_from_client()` when `message_type == HELLO`; never by the user
- UC_65 — TLS server receives HELLO and registers client NodeId — `TlsTcpBackend::handle_hello_frame()` called internally by `tls_recv_client_frame()` when `message_type == HELLO`; never by the user
- UC_66 — TCP server routes unicast frame to matched client slot — `flush_delayed_to_clients()` / `find_client_slot()` / `send_to_slot()` called internally by `TcpBackend::send_message()`; never by the user
- UC_67 — TLS server routes unicast frame to matched TLS session — `flush_delayed_to_clients()` / `find_client_slot()` / `tls_send_frame()` called internally by `TlsTcpBackend::send_message()`; never by the user
- UC_68 — Unicast send to unregistered NodeId — `find_client_slot()` returns `MAX_TCP_CONNECTIONS`; WARNING_HI logged; `ERR_INVALID` returned to caller
- UC_69 — Large message fragmentation — `DeliveryEngine::send_fragments()` + `fragment_message()` called internally by `send()` when payload > FRAG_MAX_PAYLOAD_BYTES; never invoked directly by the user
- UC_70 — Message reassembly — `ReassemblyBuffer::ingest()` called internally by `handle_fragment_ingest()` on every wire frame received; returns ERR_AGAIN until all fragments arrive
- UC_71 — Ordered delivery gate — `OrderingBuffer::ingest()` + `try_release_next()` called internally by `handle_ordering_gate()` and `deliver_held_pending()`; never by the user
- UC_72 — Peer reconnect ordering reset — `drain_hello_reconnects()` / `reset_peer_ordering()` / `OrderingBuffer::reset_peer()` called automatically at the top of `receive()`; never by the user
- UC_73 — Stale reassembly slot reclamation — `ReassemblyBuffer::sweep_stale()` called internally from `sweep_ack_timeouts()`; never by the user
- UC_74 — Ordering gap sweep — `OrderingBuffer::sweep_expired_holds()` called internally by `handle_ordering_gate()` on every data-receive call; never by the user
- UC_75 — TlsSessionStore save/load — `try_save_client_session()` / `try_load_client_session()` / `zeroize()` called internally by `TlsTcpBackend`; the User owns and injects the store, but the save/load mechanism is System-internal
- UC_76 — IPosixSyscalls / PosixSyscallsImpl — injectable POSIX syscall adapter used by `SocketUtils.cpp` for fault injection in tests; production code uses the singleton `PosixSyscallsImpl`; never called by the user
- UC_77 — Logger::init() — called once by the user (in `main()` or test `Setup()`) before any `LOG_*` macro is used; stores `ILogClock*`, `ILogSink*`, min_level, and PID
- UC_78 — TLS=0 build — `make TLS=0` injects `-DMESSAGEENGINE_NO_TLS`; excludes `TlsTcpBackend`, `DtlsUdpBackend`, `TlsSessionStore`, `MbedtlsOpsImpl` and the two TLS test binaries; `tls_demo` and `dtls_demo` print an error and exit
