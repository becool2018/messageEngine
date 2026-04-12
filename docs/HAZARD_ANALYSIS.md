# Software Safety Hazard Analysis, FMEA, and SC Function Classification

**Project:** messageEngine
**Standard:** NASA-STD-8719.13C (Software Safety), NASA-STD-8739.8A (Software Assurance)
**Policy reference:** CLAUDE.md §11
**Severity scale:** Cat I = Catastrophic, Cat II = Critical, Cat III = Marginal, Cat IV = Negligible

---

## §1 Software Safety Hazard Analysis

| ID | Hazard | Severity | Triggering Condition | Detection Mechanism | Mitigation |
|----|--------|----------|----------------------|---------------------|------------|
| HAZ-001 | **Wrong-node delivery** — a message is delivered to a node other than the intended destination | Cat II | `destination_id` corrupted in envelope; routing logic bug; Serializer endian error on destination field | `envelope_valid()` pre-check on every send and receive; `destination_id` validated by Serializer field-range check; `check_routing()` in `DeliveryEngine::receive()` drops misrouted messages | `envelope_valid()` enforced before any `send()`; Serializer validates and round-trips `destination_id`; `DeliveryEngine::receive()` calls `envelope_addressed_to()` via `check_routing()` and returns `ERR_INVALID` for messages whose `destination_id` does not match the local node (broadcast `destination_id == 0` is always accepted) |
| HAZ-002 | **Retry storm / link saturation** — uncontrolled retransmission floods the link, degrading or disabling communication | Cat II | ACK never received; exponential backoff not applied; `max_retries` not enforced; `sweep_ack_timeouts()` not called | `RetryManager::collect_due()` enforces `max_retries` and applies backoff; `AckTracker::sweep_expired()` caps outstanding slots; backoff capped at 60 s | `max_retries` enforced per message; exponential backoff applied and capped; `ACK_TRACKER_CAPACITY` hard-limits outstanding messages; `sweep_ack_timeouts()` called every iteration |
| HAZ-003 | **Duplicate command execution** — the same message is delivered and acted on more than once | Cat II | `DuplicateFilter` fails to record or detect a previously seen `(source_id, message_id)` pair; window size too small for retry count | `DuplicateFilter::check_and_record()` on every received message before delivery | `check_and_record()` called before `DeliveryEngine::receive()` returns a message; `DEDUP_WINDOW_SIZE` sized to cover maximum retry window; `MessageIdGen::next()` guarantees uniqueness per sender |
| HAZ-004 | **Stale message delivery** — a message past its `expiry_time` is delivered and acted on | Cat II | `timestamp_expired()` not called in receive path; `expiry_time_us` not set by sender; monotonic clock anomaly | `timestamp_expired()` called in `DeliveryEngine::receive()` before returning message | All envelopes require `expiry_time_us`; expired messages dropped and logged before delivery; `timestamp_now_us()` uses `CLOCK_MONOTONIC` to avoid wall-clock jumps |
| HAZ-005 | **Silent data corruption** — a message is delivered with a corrupted payload or incorrect header fields, with no error indication to the caller | Cat I | Serializer buffer overflow or underflow; truncated TCP frame accepted as complete; endian conversion error | Serializer validates all field ranges and returns `ERR_OVERFLOW`/`ERR_INVALID` on any violation; `tcp_recv_frame` reads exact declared length | All Serializer read/write paths bounds-check before access; `tcp_recv_frame` / `socket_recv_exact` enforce exact frame length; `ERR_` return codes propagated to caller |
| HAZ-006 | **Resource exhaustion** — a queue, retry table, or ACK tracker becomes full and silently drops messages without notifying the caller | Cat II | High send rate with slow ACK; capacity constants too small; `ERR_FULL` return ignored by caller | `ERR_FULL` returned from `RingBuffer::push()`, `AckTracker::track()`, `RetryManager::schedule()`; `WARNING_HI` logged on full | All capacity limits are compile-time constants; `ERR_FULL` returned and logged at `WARNING_HI`; callers required by Power of 10 Rule 7 to check all return values |
| HAZ-007 | **Partition masking** — ImpairmentEngine hides real connectivity loss, giving upper layers false confidence in link availability | Cat III | Impairment engine enabled without partition configuration; `is_partition_active()` not called; `ERR_IO` from `process_outbound()` ignored | `is_partition_active()` called first in every `process_outbound()` invocation; `ERR_IO` returned (not `OK`) for dropped messages | `process_outbound()` checks partition state on every outbound message; `ERR_IO` return forces caller acknowledgement; partition state logged at `WARNING_LO` |
| HAZ-008 | **Untrusted peer impersonation via incomplete certificate validation** — a TLS/DTLS handshake succeeds for a peer whose certificate does not match the expected server identity because `mbedtls_ssl_set_hostname()` was never called, or because `verify_peer=true` was used with an empty `peer_hostname`. CA-chain validation passes but hostname is unbound (CWE-297 / MitM). | Cat I | `client_connect_and_handshake()` (DTLS) or `tls_connect_handshake()` (TLS) omits `ssl_set_hostname()` call; `verify_peer=true` with empty `peer_hostname` accepted without error | **DtlsUdpBackend (SEC-001):** `client_connect_and_handshake()` calls `m_ops->ssl_set_hostname()` after `ssl_setup`; returns `ERR_IO` on failure (REQ-6.4.6). `verify_peer=true` with empty `peer_hostname` is rejected before handshake. **TlsTcpBackend (SEC-021 / DEF-018-13, commit 4cda101):** `tls_connect_handshake()` rejects `verify_peer=true` with empty `peer_hostname` before handshake begins (WARNING_HI + ERR_INVALID). Both backends now enforce the same invariant. Verified by `test_mock_client_ssl_set_hostname_fail` and `test_mock_client_ssl_set_hostname_called`. | Any certificate from the trusted CA that does not match `peer_hostname` is rejected at handshake by both TLS-capable backends. |
| HAZ-009 | Source_id spoofing via TCP/TLS connection — a connected TCP/TLS client sends an envelope claiming another node's source_id; DeliveryEngine trusts that field for ACK cancellation and retry suppression, corrupting delivery state. A second connection claiming an already-registered NodeId can also hijack the routing table entry. | Cat I | TcpBackend/TlsTcpBackend passed unvalidated envelopes to DeliveryEngine; duplicate NodeId registration not rejected in TlsTcpBackend | `validate_source_id()` checks inbound source_id against m_client_node_ids[slot]; WARNING_HI + discard on mismatch (REQ-6.1.11). Cross-slot duplicate NodeId: `handle_hello_frame()` scans all active slots; evicts new connection if NodeId already registered (TcpBackend: G-3 + F-13; TlsTcpBackend: SEC-012 / DEF-018-4, commit 4cda101). |
| HAZ-010 | Predictable message_id sequence enables ACK forgery — an attacker who can predict future message_id values can send forged ACKs, prematurely clearing AckTracker/RetryManager slots and causing the sender to believe messages were delivered when they were not (silent message loss on RELIABLE_RETRY channels). | Cat I | MessageIdGen seeded from low-entropy value in DeliveryEngine::init() | Seed derived from OS entropy (arc4random_buf on macOS, getrandom on Linux) XORed with local_id in DeliveryEngine::init() (DEF-015-2); message ID sequence is cryptographically unpredictable (REQ-5.2.4). |
| HAZ-011 | Source_id spoofing via DTLS — an authenticated DTLS peer sets the envelope source_id field to a different node's identity; DeliveryEngine trusts that field for ACK cancellation and retry suppression, corrupting delivery state (same class as HAZ-009 but via DTLS transport). | Cat I | DtlsUdpBackend::validate_source() returned true immediately for DTLS mode, leaving source_id field unchecked after MAC verification | process_hello_or_validate() registers peer NodeId from HELLO frame; subsequent data frames whose source_id does not match the registered NodeId are silently discarded with WARNING_HI (DEF-015-3, REQ-6.2.4). |
| HAZ-012 | Private key recovery from freed heap — mbedTLS pk_free() releases internal key memory without guaranteed zeroing; key material readable in freed pages via core dump, swap file, or heap-spray read. Complete TLS/DTLS session compromise. | Cat I | mbedtls_pk_free() called without prior mbedtls_platform_zeroize() in TlsTcpBackend and DtlsUdpBackend destructors | mbedtls_platform_zeroize(&m_pkey, sizeof(m_pkey)) added after mbedtls_pk_free() in both destructors (DEF-016-2/3, §7c / CWE-14). |
| HAZ-013 | PRNG divide-by-zero UB crashes process — PrngEngine::next_range(0, UINT32_MAX) computed range as uint32_t, wrapping to 0; raw % 0 is undefined behavior and typically a CPU trap. Denial of service. | Cat II | next_range() computed hi - lo + 1U as uint32_t; hi=UINT32_MAX wraps result to 0 | range computed as uint64_t (range64 = (uint64_t)hi - (uint64_t)lo + 1ULL), always ≥1; CERT INT33-C guard asserted (DEF-016-1). |
| HAZ-014 | Ordering gate permanent stall via UINT32_MAX sequence number — sweep_expired_holds() computed sequence_num + 1U as uint32_t; UINT32_MAX+1 wraps to 0 (the UNORDERED sentinel), advance_sequence() no-ops, and the gate never unblocks. | Cat II | Unsigned overflow in sweep_expired_holds() on sequence_num == UINT32_MAX | seq_next_guarded() helper wraps to 1 (not 0) on UINT32_MAX overflow, consistent with advance_next_expected(); CERT INT30-C (DEF-016-5). |
| HAZ-015 | **Source_id rotation via plaintext UDP — capacity exhaustion** — a peer at the correct IP:port sends envelopes with arbitrary `source_id` values; each novel `source_id` opens a fresh `DuplicateFilter` slot and `OrderingBuffer` peer table entry, exhausting both fixed-capacity tables and causing `ERR_FULL` drops for legitimate traffic. Prior to this fix, `UdpBackend::validate_source()` checked IP:port only and never enforced HELLO-before-data or source_id binding (same class as HAZ-011 but on the plaintext UDP path — no DTLS MAC to bound the attacker). | Cat I | `UdpBackend::recv_one_datagram()` passed envelopes with arbitrary `source_id` to `DeliveryEngine`; no HELLO registration step on the plaintext path | `UdpBackend::process_hello_or_validate()` added (mirrors `DtlsUdpBackend::process_hello_or_validate()`): first HELLO locks `m_peer_node_id`; data frames before HELLO are discarded with WARNING_HI; data frames whose `source_id` != `m_peer_node_id` are discarded with WARNING_HI. Duplicate HELLO frames are also rejected. `m_peer_node_id` and `m_peer_hello_received` reset on `close()`. Verified by `test_udp_data_before_hello_dropped`, `test_udp_source_id_rotation_rejected`, `test_udp_duplicate_hello_dropped`, `test_udp_hello_registration` (REQ-6.2.4 / REQ-6.1.8). |
| HAZ-016 | **Ordering gate permanent stall on peer reconnect** — when a peer disconnects and reconnects, `OrderingBuffer` retains the old `next_expected_seq` (e.g., 847) for that peer's slot. The new connection starts sending from seq=1; every message is discarded as `seq < next_expected`, permanently stalling the ordered delivery channel for that peer for the lifetime of the new session. | Cat II | `OrderingBuffer::ingest()` discards `seq < next_expected_seq` as a duplicate; on reconnect the new session's messages start from seq=1, which is always below the retained value from the prior session | `OrderingBuffer::reset_peer(src)` resets `next_expected_seq` to 1 and frees all held slots for `src`. `DeliveryEngine::reset_peer_ordering(src)` additionally discards any staged `m_held_pending` message belonging to `src`. `DeliveryEngine::drain_hello_reconnects()` polls `TransportInterface::pop_hello_peer()` at the top of every `receive()` call and triggers `reset_peer_ordering()` for each reconnecting peer (REQ-3.3.6). |
| HAZ-018 | **Variable-time equality comparisons on (source_id, message_id) — timing oracle** — AckTracker, DuplicateFilter, and OrderingBuffer perform equality comparisons that can short-circuit on the first differing byte. An attacker who can measure response latency can distinguish partial-match from full-match, enabling a timing oracle attack on ACK forgery and replay detection (CWE-208). | Cat I | Loopback or controlled-latency path where an attacker can issue probe messages with known prefixes and measure ACK latency differences | `ct_bytes_equal()` constant-time comparator (no short-circuit) added to all security-sensitive identity comparisons per REQ-3.2.11 | Constant-time comparator eliminates the timing side-channel; attacker cannot distinguish a prefix match from a full match regardless of identity length |
| HAZ-019 | **Integer overflow on wire-supplied length fields** — `frame_len` in `tcp_recv_frame()` and `total_payload_length` in `ReassemblyBuffer::open_slot()` are 32-bit values sourced directly from the wire. Values near UINT32_MAX bypass bounds checks (e.g., `frame_len > max` wraps after overflow) or corrupt slot calculations, enabling heap or stack overflows downstream (CWE-190). | Cat I | Malformed TCP frame or fragment datagram with length field 0xFFFFFFFF or similar near-overflow value | Ceiling guard: `frame_len > WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES` before any arithmetic; same guard in `open_slot()` per REQ-3.2.10 | Guard fires on the first arithmetic step; values exceeding the ceiling return ERR_INVALID immediately; NEVER_COMPILED_OUT_ASSERT enforces the post-condition |
| HAZ-020 | **CA certificate absent when verify_peer=true** — with no trust anchor, `MBEDTLS_SSL_VERIFY_REQUIRED` becomes a no-op and any certificate presented by a peer is accepted regardless of issuer. The TLS handshake completes successfully against an impersonator (CWE-295). | Cat I | `verify_peer=true` with `ca_file=""` in `TlsConfig`; operator misconfiguration or deployment tooling that omits the CA file | `init()` returns `ERR_IO` and logs FATAL when `verify_peer=true && ca_file.empty()` per REQ-6.3.6 | Fail-fast before any socket is opened; the impersonation window cannot be reached |
| HAZ-021 | **TlsSessionStore race condition — concurrent zeroize() and ssl_set_session()** — `TlsSessionStore::zeroize()` and `TlsTcpBackend::try_save_client_session()` / `try_load_client_session()` can race on the `mbedtls_ssl_session` struct in multi-threaded deployments. A zeroize-during-load race causes `ssl_set_session()` to operate on partially-zeroed session material, potentially completing a TLS handshake with corrupt session keys (CWE-362). | Cat I | Two threads: one calls `close()` → `zeroize()`, the other re-uses the same `TlsSessionStore` for a new connection → `try_load_client_session()` concurrently | POSIX `pthread_mutex_t` serialises all `try_save`, `try_load`, and `zeroize` accesses per REQ-6.3.10 | Mutex prevents the race; `session_valid` flag is checked under lock; no partial-state access possible |
| HAZ-022 | **Weak PRNG entropy in production DeliveryEngine (entropy source failure)** — if both `getrandom()` and `/dev/urandom` fail in `DeliveryEngine::init()`, the seed falls back to timestamp XOR local_id. On a system under attack (e.g., entropy pool depletion), `message_id` values become predictable from clock and PID, enabling ACK forgery via HAZ-010. The fallback is unguarded: the code continues without logging a fatal condition (CWE-338). | Cat I | Entropy source failure (exhausted pool, restricted sandbox, compromised kernel module) during `DeliveryEngine::init()` in a production build | FATAL + reset handler triggered if both entropy sources fail and `ALLOW_WEAK_PRNG_SEED` is not defined per REQ-5.2.6 | Fail-fast prevents operation with predictable message IDs; no ACK forgery window |
| HAZ-023 | **TCP slow-connect slot exhaustion DoS** — a client that completes the TCP 3-way handshake but never sends a HELLO frame holds a connection slot indefinitely. An attacker who opens `MAX_TCP_CONNECTIONS` such connections exhausts all server slots and permanently denies service to legitimate clients (CWE-400). | Cat II | Attacker opens `MAX_TCP_CONNECTIONS` TCP connections without sending HELLO; each slot is held until the server is restarted | Per-slot accept timestamp; slots not sending HELLO within `hello_timeout_ms` are evicted and closed with WARNING_HI per REQ-6.1.12 | Attacker cannot hold slots longer than `hello_timeout_ms`; legitimate clients always have slots available |
| HAZ-024 | **UDP wildcard peer_ip allows NodeId hijack before legitimate peer connects** — `UdpBackend` with `peer_ip="0.0.0.0"` or empty accepts HELLO frames from any sender. The first sender to send a HELLO becomes the registered peer (`m_peer_node_id`), binding to that NodeId. A legitimate peer that connects afterward is rejected as a duplicate HELLO (CWE-290). | Cat I | Attacker sends HELLO before the legitimate peer on a network path where attacker can race the legitimate peer; wildcard `peer_ip` configuration | `UdpBackend::init()` rejects wildcard `peer_ip` with ERR_INVALID and FATAL log per REQ-6.2.5 | Configuration is rejected before any socket is opened; no wildcard binding possible |
| HAZ-025 | **verify_peer=false + non-empty peer_hostname creates a silent MitM trap** — a developer or operator sets `peer_hostname="expected.host"` (intending hostname verification) but sets `verify_peer=false`. Hostname verification silently does not run. The operator believes the endpoint is verified; it is not. Any peer can connect and be accepted (CWE-297). | Cat I | Misconfiguration: `verify_peer=false` with non-empty `peer_hostname` in `TlsConfig`; UI or deployment tooling that sets hostname independently of the verify flag | `init()` returns ERR_INVALID and logs WARNING_HI when `verify_peer=false && !peer_hostname.empty()` per REQ-6.3.9 | Fail-fast prevents silent misconfiguration; operator is forced to either enable peer verification or clear the hostname |
| HAZ-017 | **TLS session material persists in caller memory after close() — caller does not call store.zeroize()** — `TlsSessionStore` holds the TLS session snapshot (including master-secret-derived material) between close() and the next init(). If the caller does not invoke `store.zeroize()` before process exit, core dump, or memory scan, session master-secret material is readable from memory. A party that recovers the session material can decrypt any recorded traffic from the resumed session. Extension of HAZ-012 to the TlsSessionStore lifetime contract (CWE-316). | Cat II | Caller forgets to call `store.zeroize()` when the TlsSessionStore goes out of scope or is reused; session material survives in stack or heap memory | `TlsSessionStore::~TlsSessionStore()` calls `zeroize()` as a destructor safety net (§7c). `TlsTcpBackend::close()` logs `WARNING_HI` when `store.session_valid == true` to alert the caller that material remains live (F-4: upgraded from WARNING_LO — system-wide impact matches CLAUDE.md §4 WARNING_HI taxonomy). `TlsSessionStore::zeroize()` uses `mbedtls_platform_zeroize()` (not `memset`) to prevent compiler elision (CWE-14, CLAUDE.md §7c). `SECURITY_ASSUMPTIONS.md §13` documents the caller contract. Verified by `test_tls_session_resumption_load_path()` which exercises explicit `store.zeroize()` on cleanup. **Cat II rationale (F-7):** impact is time-limited (session tickets expire, typically ≤24 h) and scope-limited (resumed sessions using that specific ticket only); does not enable server impersonation; direct mitigation exists (`store.zeroize()`); TLS 1.3 preserves forward secrecy for resumed sessions. Impact is less severe than HAZ-012 (private key exposure — permanent, enables impersonation, Cat I). Cat I would apply only if deployment uses TLS 1.2 exclusively on safety-critical command channels without a key rotation schedule; Cat II is correct for this general-purpose library. |

---

## §2 Failure Mode and Effects Analysis (FMEA)

### DeliveryEngine

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `send()` passes wrong `destination_id` | Message delivered to wrong node | Cat II HAZ-001 | `envelope_valid()` pre-check | Validate destination before send; assert destination not `NODE_ID_INVALID` |
| `receive()` returns expired message | Outdated command acted on | Cat II HAZ-004 | `timestamp_expired()` call | Drop and log all messages where `now_us >= expiry_time_us` |
| `receive()` returns duplicate message | Command executed twice | Cat II HAZ-003 | `DuplicateFilter::check_and_record()` | Filter applied before return; window covers full retry duration |
| `pump_retries()` not called | Retries silently starved | Cat III HAZ-002 | Server/client main loop calls each iteration | API contract; documented in DeliveryEngine header |
| `sweep_ack_timeouts()` not called | ACK slots leak; new messages blocked | Cat II HAZ-006 | Must be called each iteration | API contract; logged at `WARNING_HI` when `sweep_expired()` fires |
| MessageIdGen seed too predictable | local_id is known; attacker predicts future message_ids | Forged ACKs clear AckTracker/RetryManager slots; sender believes delivery succeeded | HAZ-010 | Seed includes timestamp_now_us() XOR local_id<<32; timestamp not known to remote attacker without sub-microsecond clock synchronization | DeliveryEngine |

### RetryManager

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `collect_due()` does not enforce `max_retries` | Unbounded retransmission | Cat II HAZ-002 | `retry_count >= max_retries` check in `collect_due()` | Slot deactivated and logged at `WARNING_HI` when `max_retries` reached |
| Backoff not advanced after retry | Constant-rate retransmission; link saturation | Cat II HAZ-002 | `advance_backoff()` called in `collect_due()` | Doubling backoff capped at 60 s per `advance_backoff()` |
| Expired slot not removed | Slot leak; `ERR_FULL` blocks new retries | Cat II HAZ-006 | `slot_has_expired()` checked before retry | Slot deactivated and logged at `WARNING_LO` on expiry |
| `schedule()` returns `ERR_FULL` ignored | Message never retried | Cat II HAZ-002 | `ERR_FULL` return code | Power of 10 Rule 7 mandates all returns checked; logged at `WARNING_LO` |
| `on_ack()` source_id mismatch — **fixed (DEF-003-1)**: `DeliveryEngine::receive()` previously passed `env.source_id` (remote ACK sender) to `on_ack()` instead of `env.destination_id` (local sender, matching the stored slot). Retry slots were never cancelled on ACK receipt; all retries ran to exhaustion. | Fixed: RetryManager slot now transitions to INACTIVE on receipt of matching ACK; no further retransmission. | Cat II HAZ-002 (resolved) | `on_ack()` now returns `OK` for valid ACKs; `ERR_INVALID` returned only for genuinely unmatched ACKs | `destination_id` used as lookup key in `DeliveryEngine::receive()` |

### AckTracker

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `track()` returns `ERR_FULL` and caller ignores | Message never ACK-tracked; retry unlimited | Cat II HAZ-002, HAZ-006 | `ERR_FULL` return | Rule 7 requires return check; `WARNING_HI` logged |
| `on_ack()` source_id mismatch — **fixed (DEF-003-1)**: `DeliveryEngine::receive()` previously passed `env.source_id` (remote ACK sender) to `on_ack()` instead of `env.destination_id` (local sender, matching the stored slot). ACKs now correctly clear tracker slots via the PENDING→ACKED transition. | Fixed: AckTracker slot now transitions PENDING→ACKED on receipt of matching ACK; slot freed on next sweep. Retry cancelled before expiry. | Cat II HAZ-002 (resolved) | `on_ack()` now returns `OK` for valid ACKs; `ERR_INVALID` returned only for genuinely unmatched ACKs | `destination_id` used as lookup key in `DeliveryEngine::receive()` |
| `cancel()` fails to free PENDING slot on send-rollback path | Slot persists; `sweep_expired()` later fires a false expired-message event, generating a spurious retry/timeout signal | Cat II HAZ-002 | `cancel()` returns `ERR_INVALID` on mismatch; `DeliveryEngine::send()` checks return value and logs at `WARNING_HI` | `cancel()` now SC (HAZ-002); `DeliveryEngine::send()` must check `cancel()` return; Power of 10 Rule 7 mandates all returns checked |
| `sweep_expired()` misses slot | Slot leaks; capacity decreases | Cat II HAZ-006 | `sweep_one_slot()` checks all states per sweep | All `ACK_TRACKER_CAPACITY` slots swept each call |

### OrderingBuffer

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `reset_peer()` not called on peer reconnect | New session's seq=1 messages discarded as `seq < next_expected`; ordered channel permanently stalled for that peer | Cat II HAZ-016 | `DeliveryEngine::drain_hello_reconnects()` calls `reset_peer_ordering()` on every HELLO event | `TransportInterface::pop_hello_peer()` queues HELLO events; `drain_hello_reconnects()` drains the queue at the top of every `receive()` call; `reset_peer()` resets `next_expected_seq` to 1 and frees held slots |
| `m_held_pending` not discarded on reset | Stale staged message from prior session delivered to caller on next `receive()` call | Cat II HAZ-001 | `reset_peer_ordering()` checks `m_held_pending.source_id == src` before clearing | `reset_peer_ordering()` invalidates `m_held_pending_valid` and logs WARNING_LO when the staged message belongs to the reconnecting peer |

### AckTracker — timing-oracle extension (HAZ-018)

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `on_ack()` uses variable-time comparison for (source_id, message_id) matching | Attacker can distinguish partial-match from full-match via ACK response latency; timing oracle enables forged-ACK attacks | Cat I HAZ-018 | Loopback or controlled-latency measurement; crafted partial-prefix probes | `ct_bytes_equal()` constant-time comparator replaces direct field equality in `on_ack()` slot scan per REQ-3.2.11 |

### DuplicateFilter

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `check_and_record()` false negative (misses duplicate) | Duplicate delivered and acted on | Cat II HAZ-003 | Window-based FIFO with `(source_id, message_id)` pairs | `DEDUP_WINDOW_SIZE` ≥ `max_retries`; uniqueness guaranteed by `MessageIdGen::next()` |
| `check_and_record()` false positive (drops unique) | Valid message silently lost | Cat III | Message ID monotonicity | Monotone IDs; window slides forward only; no wraparound in flight window |
| `init()` not called before use | Undefined state; assertions fire | Cat II | `NEVER_COMPILED_OUT_ASSERT(m_initialized)` | Called in `DeliveryEngine::init()` before any message processing |
| `is_duplicate()` early-exit on match leaks timing — HAZ-018 | Variable-time iteration allows attacker to distinguish duplicate from unique via latency measurement; timing oracle for replay detection bypass | Cat I HAZ-018 | Controlled-latency measurement; crafted probes with known (source_id, message_id) prefixes | `ct_bytes_equal()` constant-time comparator; loop must not short-circuit per REQ-3.2.11 |

### Serializer

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `serialize()` buffer overflow | Truncated or corrupt wire frame sent | Cat I HAZ-005 | Bounds check before every write; returns `ERR_OVERFLOW` | Fixed-layout serialization; `SOCKET_RECV_BUF_BYTES` sized for max envelope |
| `deserialize()` reads beyond buffer | Corrupt fields reconstructed silently | Cat I HAZ-005 | `offset + sizeof(field) > len` guard on every read | Returns `ERR_INVALID` on any out-of-bounds read |
| Endian conversion error | Field interpreted with wrong byte order | Cat I HAZ-005 | Fixed big-endian encoding; deterministic layout | `write_u32` / `read_u32` always swap; no host-endian assumptions |
| Payload length mismatch | Truncated payload delivered | Cat I HAZ-005 | `payload_length` field vs. actual bytes copied | `payload_length <= MSG_MAX_PAYLOAD_BYTES` enforced in both directions |
| Protocol version or magic mismatch silently accepted | Frame from incompatible wire format decoded with wrong field layout; corrupt data delivered to caller | Cat I HAZ-005 | `proto_ver != PROTO_VERSION` check at byte 3; `magic_word != (PROTO_MAGIC << 16)` check at bytes 40–43 in `deserialize()` | `ERR_INVALID` returned immediately on version or magic mismatch before any field is read; logged at `WARNING_HI` (REQ-3.2.8) |

### DeliveryEngine — entropy-fallback extension (HAZ-022)

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `init()` entropy sources (getrandom / /dev/urandom) both fail without fatal guard | `MessageIdGen` seeded with low-entropy value; message IDs become predictable from clock + PID; ACK forgery enabled (HAZ-010 extended) | Cat I HAZ-022 | Entropy source failure in restricted sandbox or under entropy-pool attack | FATAL + reset handler if both sources fail and `ALLOW_WEAK_PRNG_SEED` undefined per REQ-5.2.6 |

### TcpBackend / UdpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| Partial TCP frame accepted as complete message | Corrupt fields delivered | Cat I HAZ-005 | `tcp_recv_frame` uses `socket_recv_exact` | `socket_recv_exact` loops until all bytes received or error |
| `frame_len` near UINT32_MAX bypasses bounds check (HAZ-019) | Arithmetic overflow after check; oversized allocation or buffer overrun | Cat I HAZ-019 | Wire-level injection of malformed frame with near-UINT32_MAX length field | Ceiling guard: `frame_len > WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES` → ERR_INVALID + WARNING_HI per REQ-3.2.10 |
| `send_message()` called with no connected client | Message silently discarded | Cat III HAZ-006 | `m_client_count == 0` check; `WARNING_LO` logged | Logged; upper layer should verify connectivity before sending |
| `receive_message()` returns `ERR_TIMEOUT` spuriously | Upper layer delays message processing | Cat IV | Bounded `poll_count` loop | Caller retries; `RECV_TIMEOUT_MS` tunable per channel |
| validate_source_id() fails (allows spoofed frame through) | Source_id mismatch not caught | DeliveryEngine processes forged frame | HAZ-009 | validate_source_id() verifies m_client_node_ids[slot] before dispatch; WARNING_HI on mismatch | TcpBackend |
| TCP client holds slot without sending HELLO — slow-connect DoS (HAZ-023) | All `MAX_TCP_CONNECTIONS` slots occupied by unauthenticated connections; legitimate clients denied service | Cat II HAZ-023 | No client traffic for `hello_timeout_ms` after accept; slot never evicted without timeout guard | Per-slot accept timestamp; evict slots without HELLO after `hello_timeout_ms` per REQ-6.1.12 |
| UdpBackend configured with wildcard `peer_ip` (HAZ-024) | First sender to HELLO becomes registered peer; legitimate peer denied registration | Cat I HAZ-024 | `peer_ip=""` or `"0.0.0.0"` accepted in init() | `init()` rejects wildcard `peer_ip` with ERR_INVALID + FATAL per REQ-6.2.5 |
| **Non-HELLO data frame accepted from unregistered slot (Fix 3 — fixed)** — `recv_from_client()` previously passed data frames to DeliveryEngine before a HELLO had been received for that slot, allowing data injection before NodeId binding. Tied to HAZ-009 / REQ-6.1.11. | Forged source_id reaches DeliveryEngine before routing table entry is established; ACK cancellation or dedup state corrupted | Cat I HAZ-009 | Absence of registration check; frame accepted without slot NodeId binding | **Fixed (commit 4c714bd, Fix 3):** `is_unregistered_slot()` rejects any non-HELLO frame from a slot where `m_client_node_ids[slot] == NODE_ID_INVALID`; WARNING_HI logged; frame silently discarded. |
| **HELLO replay mid-session allows NodeId hijack (Fix 4 — fixed)** — A second HELLO from an already-registered slot was accepted, silently overwriting the routing-table entry and enabling NodeId substitution. Tied to HAZ-009. | Attacker replaces binding for a live slot; subsequent frames are attributed to wrong NodeId; ACK/retry state corrupted | Cat I HAZ-009 | No guard on duplicate HELLO; routing table entry mutable after first registration | **Fixed (commit 4c714bd, Fix 4):** `m_client_hello_received[]` added; `process_hello_frame()` rejects duplicate HELLO with WARNING_HI; routing table entry immutable after first registration. |

### TlsTcpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| TLS handshake failure (bad cert/key) | Connection not established; no messages exchanged | Cat III | `mbedtls_ssl_handshake()` returns non-zero; logged at `WARNING_HI`; `init()` returns `ERR_IO` | Certificate and key loaded and validated in `setup_tls_config()` before bind/connect; `ERR_IO` forces caller to abort |
| TLS cert/key files missing or corrupt | `init()` fails; transport not opened | Cat III | `mbedtls_x509_crt_parse_file()` / `mbedtls_pk_parse_keyfile()` return non-zero; `ERR_IO` returned | `is_open()` returns `false`; upper layer must check `init()` result |
| Plaintext mode used when TLS expected | Unencrypted traffic on secured link | Cat II | `tls_enabled` flag controls code path | Configuration-level control; operator responsibility to set `tls_enabled=true` when security is required |
| Partial TLS record delivered | Corrupt fields reconstructed silently | Cat I HAZ-005 | `mbedtls_ssl_read()` returns exact byte count; 4-byte length prefix checked | `tls_recv_frame()` validates payload length against header; rejects oversized frames |
| **recv called on idle connection without readability check (B-2a — fixed)** — `poll_clients_once()` called `recv_from_client()` for every slot without checking socket readability, causing spurious wakes and undefined behavior on idle connections (HAZ-004, HAZ-005) | Spurious receive attempts on idle sockets; potential premature close of idle connections | Cat II HAZ-004, HAZ-005 | Absence of `POLLIN` guard; visible in socket-error returns from `recv_from_client()` | **Fixed (commit 326d7be, B-2a):** `poll_clients_once()` now builds a `pollfd` array and calls `poll()` once with `timeout_ms`; `recv_from_client()` gated on `POLLIN` or TLS internal-bytes flag per slot. Prevents spurious close of idle connections. |
| **Accepted TLS socket left permanently blocking after handshake (B-2b — fixed)** — `accept_and_handshake()` called `mbedtls_net_set_block()` before handshake but never restored non-blocking mode afterward; all accepted client sockets were permanently blocking, blocking the receive loop (HAZ-004) | `receive_message()` blocks indefinitely on any client socket that has no data pending; violates multi-connection and timeout contract (REQ-6.1.3, REQ-6.1.7) | Cat II HAZ-004 | Indefinite block in `poll_clients_once()` on a connected-but-idle client socket | **Fixed (commit 326d7be, B-2b):** New helper `do_tls_server_handshake(uint32_t slot)` calls `mbedtls_net_set_nonblock()` immediately after handshake completion before returning to the accept loop. All accepted sockets are now non-blocking. |
| **Timeout ignored on TLS WANT_READ path (B-2c — fixed)** — `tls_read_payload()` honored `timeout_ms` on the plaintext path but looped indefinitely on `MBEDTLS_ERR_SSL_WANT_READ` on the TLS path, violating the caller's timeout contract (HAZ-004) | `receive_message()` never times out on a connected TLS socket that stops sending mid-frame; upper layer permanently stalled | Cat II HAZ-004 | No timeout return from `tls_recv_frame()` on TLS path; caller's `timeout_ms` silently ignored | **Fixed (commit 326d7be, B-2c):** New helper `read_tls_header()` and updated `tls_read_payload()` call `mbedtls_net_poll()` on `WANT_READ` with the caller-supplied `timeout_ms`; return `ERR_TIMEOUT` if the deadline is exceeded. `receive_message()` now always honors its `timeout_ms` contract. |
| validate_source_id() fails (allows spoofed frame through) | Source_id mismatch not caught | DeliveryEngine processes forged frame | HAZ-009 | validate_source_id() verifies m_client_node_ids[slot] before dispatch; WARNING_HI on mismatch | TlsTcpBackend |
| **Cipher suite downgrade not blocked (Fix 1 — fixed)** — `setup_tls_config()` did not restrict negotiated ciphers; a peer could negotiate CBC, NULL, or RSA key-exchange ciphers, undermining transport confidentiality and integrity. | Encrypted channel negotiated with weak cipher; confidentiality and integrity guarantees absent; HAZ-005 mitigation weakened | Cat I HAZ-005 | Absence of cipher allowlist call; negotiated suite visible only in mbedTLS debug log | **Fixed (commit 0057d56, Fix 1):** `mbedtls_ssl_conf_ciphersuites()` called with AEAD-only allowlist; `mbedtls_ssl_conf_min_tls_version()` enforces TLS 1.2 minimum. CBC, RSA key exchange, and NULL ciphers are rejected at handshake time. |
| **Session ticket key not initialized (Fix 2 — fixed)** — Session resumption was wired but `mbedtls_ssl_ticket_setup()` was never called; the ticket context lacked a server key, leaving session ticket encryption undefined. | Resumed sessions may use uninitialized or predictable key material; session confidentiality not guaranteed | Cat II | Session resumption active but ticket context in undefined state; failure mode latent until a client attempts resumption | **Fixed (commit 0057d56, Fix 2):** `maybe_setup_session_tickets()` calls `mbedtls_ssl_ticket_setup()` with configured `session_ticket_lifetime_s`; ticket context freed and re-inited in `close()` and destructor per CLAUDE.md §7c. |
| **Non-HELLO data frame accepted from unregistered slot (Fix 3 — fixed)** — `recv_from_client()` previously passed data frames to DeliveryEngine before a HELLO had been received for that slot. Tied to HAZ-009 / REQ-6.1.11. | Forged source_id reaches DeliveryEngine before routing table entry is established | Cat I HAZ-009 | Absence of registration check | **Fixed (commit 0057d56, Fix 3):** `classify_inbound_frame()` rejects any non-HELLO frame from a slot where `m_client_node_ids[slot] == NODE_ID_INVALID`; WARNING_HI logged; frame discarded. |
| **HELLO replay mid-session allows NodeId hijack (Fix 4 — fixed)** — A second HELLO from an already-registered slot was accepted, overwriting the routing-table entry. Tied to HAZ-009. | Attacker replaces binding for a live slot; ACK/retry state corrupted | Cat I HAZ-009 | No guard on duplicate HELLO | **Fixed (commit 0057d56, Fix 4):** `m_client_hello_received[]` added; `classify_inbound_frame()` rejects duplicate HELLO with WARNING_HI; entry immutable after first registration. |
| **ssl_context shallow copy causes undefined behavior on slot compaction (Fix 5 — fixed)** — `remove_client()` used array compaction (memmove / struct assignment) on `m_ssl[]`; bitwise copy of opaque `mbedtls_ssl_context` is prohibited by the mbedTLS API and produces use-after-free of internal pointers. | Undefined behavior on any client disconnect; internal ssl context pointers invalid after move; potential memory corruption and arbitrary code execution | Cat I HAZ-005 | Defect triggered on every client disconnect that leaves a gap in the slot table; no runtime signal in normal operation | **Fixed (commit 0057d56, Fix 5):** `m_client_slot_active[MAX_TCP_CONNECTIONS]` flag array added. `remove_client()` marks slot inactive and calls `mbedtls_ssl_free()` + `mbedtls_ssl_init()` in-place; all iteration loops skip inactive slots. No ssl context is ever copied or moved. |
| **CA certificate absent with verify_peer=true (HAZ-020)** — `setup_tls_config()` or `init()` accepted `verify_peer=true` without checking that `ca_file` is non-empty; `MBEDTLS_SSL_VERIFY_REQUIRED` is set but mbedTLS accepts any certificate when the CA trust store is empty. | MitM attacker with any certificate is accepted; encrypted channel established to wrong peer; HAZ-005 mitigation bypassed | Cat I HAZ-020 | `init()` does not fail; TLS handshake completes silently with empty CA | `init()` returns ERR_IO + FATAL when `verify_peer=true && ca_file.empty()` per REQ-6.3.6 |
| **require_crl=true with empty crl_file accepted silently** — when `require_crl=true` and `crl_file` is empty, no CRL is loaded but the verify flag is armed; revoked certificates cannot be detected. | Revoked peer certificate accepted; attacker with a stolen revoked certificate can authenticate | Cat II | No CRL loaded; no runtime signal | `init()` returns ERR_INVALID + FATAL when `require_crl=true && verify_peer=true && crl_file.empty()` per REQ-6.3.7 |
| **tls_require_forward_secrecy not enforced on TLS 1.2 session resumption** — TLS 1.2 session resumption bypasses the full handshake; a resumed session does not provide forward secrecy even when `tls_require_forward_secrecy=true`. | Resumed TLS 1.2 sessions lack forward secrecy; recorded traffic decryptable if session master secret is later compromised | Cat II | Resumed session accepted without FS check | When `tls_require_forward_secrecy=true`, zeroize and reject session resumption after negotiation per REQ-6.3.8 |
| **verify_peer=false + non-empty peer_hostname — silent MitM trap (HAZ-025)** | Operator believes hostname is verified; it is not; any peer accepted | Cat I HAZ-025 | No error or warning on misconfigured combination | `init()` returns ERR_INVALID + WARNING_HI when `!verify_peer && !peer_hostname.empty()` per REQ-6.3.9 |
| **Cross-slot duplicate NodeId not rejected in TlsTcpBackend (SEC-012 — fixed)** — `handle_hello_frame()` did not scan other active slots before recording a new NodeId; a second TLS connection could claim an already-registered NodeId, overwriting the routing table entry and hijacking the active peer's routing slot. Tied to HAZ-009. | Second attacker connection silently replaces legitimate peer's NodeId binding; subsequent frames attributed to attacker; ACK/retry state corrupted | Cat I HAZ-009 | No cross-slot scan in `handle_hello_frame()`; defect latent until two connections claim the same NodeId | **Fixed (SEC-012 / DEF-018-4, commit 4cda101):** `handle_hello_frame()` now scans all active slots for NodeId collision; evicts the **new** connection (calls `remove_client(idx_new)`) if the NodeId is already registered elsewhere; existing binding preserved. Mirrors TcpBackend G-3 + F-13. |
| **TLS client accepted verify_peer=true with empty peer_hostname (SEC-021 — fixed / CWE-297)** — `tls_connect_handshake()` proceeded with the TLS handshake even when `verify_peer=true` and `peer_hostname` was empty. CA-chain validation succeeded but no CN/SAN binding was enforced, allowing any certificate from the trusted CA to be accepted — a MitM window. Same defect class as SEC-001 in DtlsUdpBackend. Tied to HAZ-008. | MitM attacker with a certificate from the same trusted CA can impersonate the server; encrypted session established to the wrong peer | Cat I HAZ-008 | `verify_peer=true` with empty `peer_hostname` accepted without error; no hostname binding before handshake | **Fixed (SEC-021 / DEF-018-13, commit 4cda101):** `tls_connect_handshake()` rejects `verify_peer=true` with empty `peer_hostname` before the handshake begins (WARNING_HI + `ERR_INVALID`). Both TLS-capable backends (TlsTcpBackend and DtlsUdpBackend) now enforce identical invariants per SECURITY_ASSUMPTIONS.md §8/§11. |

### DtlsUdpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| DTLS handshake failure (bad cert/key) | Connection not established; no messages exchanged | Cat III | `mbedtls_ssl_handshake()` returns non-zero; logged at `WARNING_HI`; `init()` returns `ERR_IO` | Cert/key validated in `setup_dtls_config()` before handshake; `ERR_IO` forces caller to abort |
| DTLS cert/key files missing or corrupt | `init()` fails; transport not opened | Cat III | `mbedtls_x509_crt_parse_file()` / `mbedtls_pk_parse_keyfile()` return non-zero | `is_open()` returns `false`; upper layer must check `init()` result |
| Plaintext mode used when DTLS expected | Unencrypted traffic on secured link | Cat II | `tls_enabled` flag; if `false`, plaintext UDP path taken (REQ-6.4.5) | Operator responsibility to set `tls_enabled=true`; same pattern as TlsTcpBackend |
| DTLS cookie exchange bypass | Amplification attack vector; unverified client | Cat II | `mbedtls_ssl_conf_dtls_cookies()` armed in server `setup_dtls_config()`; cookie checked on every ServerHello | Cookie context initialised and armed before any handshake; `HELLO_VERIFY_REQUIRED` handled in `run_dtls_handshake()` (REQ-6.4.2) |
| Serialized payload exceeds DTLS MTU | IP fragmentation or send failure | Cat III HAZ-005 | `wire_len > DTLS_MAX_DATAGRAM_BYTES` check in `send_message()`; returns `ERR_INVALID` | Hard check before every send; `mbedtls_ssl_set_mtu()` configures DTLS record MTU (REQ-6.4.4) |
| DTLS handshake timeout (server never receives first datagram) | `init()` returns `ERR_TIMEOUT`; transport not opened | Cat III | `poll()` in `server_wait_and_handshake()` times out after `connect_timeout_ms` | `ERR_TIMEOUT` propagated to caller; upper layer must retry or abort |
| HAZ-008: `ssl_set_hostname()` not called before handshake | Impersonation / MitM — encrypted session to wrong peer | Cat I | mbedTLS returns handshake error on CN/SAN mismatch (when `verify_peer=true`) | REQ-6.4.6 — `ssl_set_hostname()` called after `ssl_setup` in client path |
| **Cipher suite downgrade not blocked (Fix 1 — fixed)** — `setup_dtls_config()` did not restrict negotiated ciphers; a peer could negotiate CBC, NULL, or RSA key-exchange ciphers. | Encrypted DTLS channel with weak cipher; confidentiality and integrity guarantees absent; HAZ-005 mitigation weakened | Cat I HAZ-005 | Absence of cipher allowlist call | **Fixed (commit ca961de, Fix 1):** `mbedtls_ssl_conf_ciphersuites()` called with AEAD-only allowlist; `mbedtls_ssl_conf_min_tls_version()` enforces TLS 1.2 minimum. |

### TlsSessionStore

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| **Concurrent zeroize() and try_save/try_load race (HAZ-021)** — `zeroize()` and `try_save_client_session()` / `try_load_client_session()` race on the `mbedtls_ssl_session` struct in multi-threaded deployments; a zeroize-during-load produces a TLS handshake using partially-zeroed session keys | TLS handshake completes with corrupt session material; encrypted channel may use known or guessable key material | Cat I HAZ-021 | Data race detected by thread sanitizer; race window is small (~μs) but deterministically reproducible under a two-thread test | POSIX `pthread_mutex_t` serialises all `try_save`, `try_load`, and `zeroize` accesses; `session_valid` checked under lock per REQ-6.3.10 |
| Caller does not invoke `store.zeroize()` before scope exit / process shutdown | TLS session master-secret-derived material persists in stack or heap memory; recoverable from core dump, swap file, or heap scan (CWE-316) | Cat II HAZ-017 | `TlsSessionStore::~TlsSessionStore()` calls `zeroize()` as destructor safety net; `TlsTcpBackend::close()` logs `WARNING_HI` when `session_valid == true` after close | `mbedtls_platform_zeroize()` called in destructor and in `zeroize()` (compiler-barrier prevents elision); `SECURITY_ASSUMPTIONS.md §13` documents caller contract |
| `zeroize()` called while mbedTLS holds internal allocations in the session struct | Internal pointers in freed session memory point to already-freed storage; subsequent `mbedtls_ssl_session_init()` overwrites dangling pointers, leaving the struct in a defined-safe state | Cat II HAZ-017 | `zeroize()` calls `mbedtls_ssl_session_free()` before `mbedtls_platform_zeroize()` to release any ticket or cert chain allocations first | `zeroize()` order: (1) `session_free`, (2) `platform_zeroize`, (3) `session_init`; order is documented and asserted via `NEVER_COMPILED_OUT_ASSERT(!session_valid)` post-condition |
| `TlsSessionStore` copied or moved (mbedTLS internal pointer aliasing) | Internal pointers aliased across two sessions; `mbedtls_ssl_session_free()` on one copy double-frees allocations owned by the other | Cat I HAZ-005 | Copy and move constructors and assignment operators are `= delete`; attempt to copy or move fails at compile time | All four special members (`TlsSessionStore(const TlsSessionStore&)`, `operator=`, move ctor, move assign) explicitly deleted; no workaround possible without cast |

### IMbedtlsOps / MbedtlsOpsImpl

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `MbedtlsOpsImpl` method returns wrong error code | TLS/DTLS state machine misinterprets handshake step; connection not established or silently insecure | Cat II HAZ-005 | `DtlsUdpBackend` checks every return value; `WARNING_HI` logged on non-zero return | All mbedTLS return codes mapped to `bool`; caller checks `false` and propagates `ERR_IO` |
| Mock (`MockMbedtlsOps`) used in production build | No real TLS; cleartext DTLS traffic | Cat I HAZ-005 | Link-time symbol resolution; `IMbedtlsOps*` injection via `init()` parameter | `MbedtlsOpsImpl` is the only concrete implementation linked in production; `MockMbedtlsOps` is test-only (`tests/`) |
| `psa_crypto_init()` failure not detected | DTLS entropy pool absent; handshake keys derived from uninitialized state | Cat I HAZ-005 | `ops->psa_crypto_init()` checked in `DtlsUdpBackend::init()`; returns `ERR_IO` on failure | Any non-zero return aborts `init()`; `is_open()` returns `false` |
| vtable pointer overwritten (memory corruption) | Arbitrary code execution via corrupted dispatch | Cat I | Address-space protection (ASLR, stack canaries); read-only vtable segment | `IMbedtlsOps` interface is const-pointer-to-interface; no writable function pointer in app code |

### ImpairmentEngine

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `process_outbound()` skips partition check | Partition not simulated; real link loss masked | Cat III HAZ-007 | `is_partition_active()` called first in `process_outbound()` | First check in function; cannot be bypassed |
| `check_loss()` uses uninitialized PRNG | Non-deterministic / always-pass behavior | Cat III HAZ-007 | `NEVER_COMPILED_OUT_ASSERT(m_initialized)` | `init()` must be called; assertion fires if not |
| Delay buffer full; message silently dropped | HAZ-006 | Cat II | `ERR_FULL` returned; `WARNING_HI` logged | `m_delay_count < IMPAIR_DELAY_BUF_SIZE` checked before `queue_to_delay_buf()` |
| **Double-send when `enabled == false`** — `process_outbound()` queues message to delay buffer with `release_us = now_us`; if a caller both invokes a direct send AND then calls `collect_deliverable()`, the message is sent twice | Every message transmitted twice; duplicate arrives at receiver; DuplicateFilter may suppress second copy for RELIABLE_RETRY but not for BEST_EFFORT | Cat II HAZ-003 | Visible in logs as two sends per call; DuplicateFilter counters | **Fixed in TcpBackend** (direct `send_to_all_clients()` call removed from `send_message()`; messages sent only via `flush_delayed_to_clients()`). `LocalSimHarness` must similarly ensure it does not double-send. DuplicateFilter mitigates residual risk for RELIABLE_RETRY traffic. |
| Duplicate injected at wrong offset | Double-delivery from wrong source | Cat II HAZ-003 | `envelope_copy` preserves source metadata | Duplicate carries original `source_id` / `message_id`; DuplicateFilter catches it |

---

## §3 Safety-Critical Function Classification

**Classification criteria:**
- **SC (Safety-Critical):** directly on the send/receive/expiry/dedup/retry/impairment path; removal or failure would directly trigger a HAZ-NNN entry.
- **NSC (Non-Safety-Critical):** observability, configuration, lifecycle initialisation, or test-only; no direct effect on message delivery semantics.

SC functions must carry `// Safety-critical (SC): HAZ-NNN` in their `.hpp` declaration (CLAUDE.md §11 rule 2).

### src/core/MessageEnvelope.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `envelope_init()` | — | SC | HAZ-005 |
| `envelope_copy()` | — | SC | HAZ-002, HAZ-003 |
| `envelope_valid()` | — | SC | HAZ-001, HAZ-005 |
| `envelope_is_data()` | — | SC | HAZ-001, HAZ-004 |
| `envelope_is_control()` | — | SC | HAZ-001 |
| `envelope_make_ack()` | — | SC | HAZ-002 |
| `envelope_needs_ack_response()` | — | SC | HAZ-002 |
| `envelope_addressed_to()` | — | SC | HAZ-001 |

`envelope_needs_ack_response()` is SC (HAZ-002): it determines whether an inbound message requires an ACK reply; an incorrect result causes a missing ACK, which leaves the sender in PENDING state indefinitely, triggering the retry-storm hazard. `envelope_addressed_to()` is SC (HAZ-001): it is the routing decision helper used by `DeliveryEngine::receive()` via `check_routing()`; an incorrect result allows misrouted delivery to reach the wrong node.

### src/core/MessageId.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `MessageIdGen` | SC | HAZ-003 |
| `next()` | `MessageIdGen` | SC | HAZ-003 |

### src/core/Timestamp.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `timestamp_now_us()` | — | SC | HAZ-004 |
| `timestamp_expired()` | — | SC | HAZ-004 |
| `timestamp_deadline_us()` | — | SC | HAZ-002, HAZ-004 |

### src/core/Serializer.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `serialize()` | `Serializer` | SC | HAZ-005 |
| `deserialize()` | `Serializer` | SC | HAZ-001, HAZ-005, HAZ-019 |

### src/core/RingBuffer.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `RingBuffer` | NSC | — |
| `push()` | `RingBuffer` | SC | HAZ-006 |
| `pop()` | `RingBuffer` | SC | HAZ-001, HAZ-004 |

### src/core/AckTracker.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `AckTracker` | NSC | — |
| `track()` | `AckTracker` | SC | HAZ-002, HAZ-006 |
| `on_ack()` | `AckTracker` | SC | HAZ-002, HAZ-018 |
| `cancel()` | `AckTracker` | SC | HAZ-002 |
| `sweep_expired()` | `AckTracker` | SC | HAZ-002, HAZ-006 |
| `get_send_timestamp()` | `AckTracker` | NSC | — |
| `get_tracked_destination()` | `AckTracker` | SC | HAZ-002, HAZ-018 |
| `get_stats()` | `AckTracker` | NSC | — |

`get_tracked_destination()` is SC (HAZ-002): forge-ACK prevention — it verifies that the destination NodeId carried in an inbound ACK matches the tracked sender stored in the slot; a mismatch must be rejected to prevent forged ACKs from prematurely clearing retry slots and causing the sender to believe a message was delivered when it was not.

### src/core/DuplicateFilter.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DuplicateFilter` | NSC | — |
| `is_duplicate()` | `DuplicateFilter` | SC | HAZ-003, HAZ-018 |
| `record()` | `DuplicateFilter` | SC | HAZ-003 |
| `check_and_record()` | `DuplicateFilter` | SC | HAZ-003, HAZ-018 |

### src/core/RetryManager.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `RetryManager` | NSC | — |
| `schedule()` | `RetryManager` | SC | HAZ-002 |
| `on_ack()` | `RetryManager` | SC | HAZ-002 |
| `collect_due()` | `RetryManager` | SC | HAZ-002 |
| `cancel()` | `RetryManager` | NSC | — |
| `get_stats()` | `RetryManager` | NSC | — |

`cancel()` is NSC: bookkeeping correction only; it deactivates a scheduled retry slot when the caller no longer needs it (e.g., on send rollback), but it does not itself determine whether a message is delivered or retried — it only prevents a future spurious retry attempt. No delivery state change occurs as a direct result of `cancel()`.

### src/core/Fragmentation.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `needs_fragmentation()` | — | NSC | — |
| `fragment_message()` | — | SC | HAZ-005 |

Rationale: Fragment splitting is a wire-format operation; incorrect splitting corrupts the deserialized message (HAZ-005). HAZ-001 and HAZ-006 do not directly apply — the fragmentation function does not route or exhaust message-ring capacity.

### src/core/ReassemblyBuffer.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `ReassemblyBuffer` | NSC | — |
| `ingest()` | `ReassemblyBuffer` | SC | HAZ-003, HAZ-006, HAZ-019 |
| `sweep_expired()` | `ReassemblyBuffer` | NSC | — |
| `sweep_stale()` | `ReassemblyBuffer` | NSC | — |

`sweep_stale()` is NSC: housekeeping sweep that reclaims reassembly slots open longer than `recv_timeout_ms` without a completed fragment set (REQ-3.2.9); it prevents resource exhaustion from peers that send partial fragment sets, but it does not itself make any delivery decision — it only frees incomplete slots that would otherwise never complete.

### src/core/OrderingBuffer.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `OrderingBuffer` | NSC | — |
| `ingest()` | `OrderingBuffer` | SC | HAZ-001, HAZ-006, HAZ-018 |
| `try_release_next()` | `OrderingBuffer` | SC | HAZ-001 |
| `reset_peer()` | `OrderingBuffer` | SC | HAZ-001, HAZ-016 |
| `advance_sequence()` | `OrderingBuffer` | NSC | — |
| `sweep_expired_holds()` | `OrderingBuffer` | SC | HAZ-001, HAZ-014 |

### src/core/DeliveryEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DeliveryEngine` | SC | HAZ-022 |
| `send()` | `DeliveryEngine` | SC | HAZ-001, HAZ-002, HAZ-006 |
| `receive()` | `DeliveryEngine` | SC | HAZ-001, HAZ-003, HAZ-004, HAZ-005 |
| `pump_retries()` | `DeliveryEngine` | SC | HAZ-002 |
| `sweep_ack_timeouts()` | `DeliveryEngine` | SC | HAZ-002, HAZ-006 |
| `reset_peer_ordering()` | `DeliveryEngine` | SC | HAZ-001, HAZ-016 |
| `drain_hello_reconnects()` | `DeliveryEngine` | NSC | — |
| `get_stats()` | `DeliveryEngine` | NSC | — |

### src/core/TransportInterface.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TransportInterface` | NSC | — |
| `send_message()` | `TransportInterface` | SC | HAZ-001, HAZ-005, HAZ-006 |
| `receive_message()` | `TransportInterface` | SC | HAZ-001, HAZ-004, HAZ-005 |
| `register_local_id()` | `TransportInterface` | NSC | — |
| `pop_hello_peer()` | `TransportInterface` | NSC | — |
| `close()` | `TransportInterface` | NSC | — |
| `is_open()` | `TransportInterface` | NSC | — |
| `get_transport_stats()` | `TransportInterface` | NSC | — |

`register_local_id()` is a non-pure virtual with a default no-op implementation in `TransportInterface`. It is called by `DeliveryEngine::init()` after transport init to propagate the local `NodeId` to backends that implement unicast routing (currently `TcpBackend` and `TlsTcpBackend`). Classification: NSC — it carries no message-delivery policy itself; the SC behavior is encoded in the concrete backend implementations.

### src/core/Types.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `result_ok()` | — | NSC | — |

### src/core/Logger.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `log()` | `Logger` | NSC | — |

### src/core/ChannelConfig.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `channel_config_default()` | — | NSC | — |
| `transport_config_default()` | — | NSC | — |

### src/platform/PrngEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `seed()` | `PrngEngine` | SC | HAZ-002, HAZ-007 |
| `next()` | `PrngEngine` | SC | HAZ-002, HAZ-007 |
| `next_double()` | `PrngEngine` | SC | HAZ-002, HAZ-007 |
| `next_range()` | `PrngEngine` | SC | HAZ-002, HAZ-007, HAZ-013 |

### src/platform/ImpairmentConfig.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `impairment_config_default()` | — | NSC | — |

### src/platform/ImpairmentConfigLoader.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `impairment_config_load()` | — | SC | HAZ-002, HAZ-007 |

Rationale: `impairment_config_load()` sets `loss_probability`, `partition_enabled`, `partition_duration_ms`, and related fields that directly govern whether HAZ-002 (retry storm / link saturation) and HAZ-007 (partition masking) mitigations are armed. A malformed config file that sets `enabled=0` silently disables all impairments, masking real connectivity problems (HAZ-007). A config that omits `max_retries`-related clamping fields could feed unchecked values to `ImpairmentEngine::init()`. It is classified SC for init-phase safety.

### src/platform/ImpairmentEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `ImpairmentEngine` | NSC | — |
| `process_outbound()` | `ImpairmentEngine` | SC | HAZ-002, HAZ-007 |
| `collect_deliverable()` | `ImpairmentEngine` | SC | HAZ-004, HAZ-006 |
| `process_inbound()` | `ImpairmentEngine` | SC | HAZ-003, HAZ-007 |
| `is_partition_active()` | `ImpairmentEngine` | SC | HAZ-007 |
| `config()` | `ImpairmentEngine` | NSC | — |
| `get_stats()` | `ImpairmentEngine` | NSC | — |

### src/platform/SocketUtils.hpp

All `SocketUtils` functions are **NSC** — raw POSIX I/O primitives with no message-delivery policy. Failure returns are propagated to callers; no silent discard occurs at this layer.

### src/platform/TcpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TcpBackend` | SC | HAZ-023 |
| `send_message()` | `TcpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `TcpBackend` | SC | HAZ-004, HAZ-005 |
| `register_local_id()` | `TcpBackend` | NSC | — |
| `send_hello_frame()` | `TcpBackend` | NSC | — |
| `find_client_slot()` | `TcpBackend` | NSC | — |
| `handle_hello_frame()` | `TcpBackend` | NSC | — |
| `recv_from_client()` | `TcpBackend` | SC | HAZ-009, HAZ-023 |
| `evict_hello_timeout_slots()` | `TcpBackend` | SC | HAZ-023 |
| `send_to_slot()` | `TcpBackend` | NSC | — |
| `validate_source_id()` | `TcpBackend` | SC | HAZ-009 |
| `close()` | `TcpBackend` | NSC | — |
| `is_open()` | `TcpBackend` | NSC | — |
| `get_transport_stats()` | `TcpBackend` | NSC | — |

Rationale for new NSC functions: `register_local_id()` and `send_hello_frame()` are called
once during init to advertise the local NodeId to the server. A failure here (HELLO frame
not sent) means the server cannot build a routing table for this client, which could result
in broadcast-only delivery rather than unicast delivery. This is a connectivity concern
(similar in severity to HAZ-006 resource-exhaustion) but does not directly trigger silent
data corruption (HAZ-005) or wrong-node delivery (HAZ-001) — the server falls back to
broadcast (destination_id == NODE_ID_INVALID behavior). Classification: NSC. If future
analysis determines that failed HELLO causes HAZ-001 (misroute/no-delivery), reclassify
`register_local_id()` and `send_hello_frame()` as SC: HAZ-001, HAZ-006 and add a HAZ
entry for "HELLO failure causes misroute or no-delivery."

`find_client_slot()`, `handle_hello_frame()`, and `send_to_slot()` are routing-table
maintenance helpers. `find_client_slot()` is called within `flush_delayed_to_clients()`
(already SC); however, its own function body performs only an O(C) linear scan of a fixed
array — it does not itself encode any message-delivery policy beyond slot index lookup.
Classification: NSC. `flush_delayed_to_clients()` as a whole remains SC (HAZ-005, HAZ-006).

### src/platform/UdpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `UdpBackend` | SC | HAZ-024 |
| `register_local_id()` | `UdpBackend` | SC | HAZ-005 |
| `send_hello_datagram()` | `UdpBackend` | SC | HAZ-005 |
| `send_message()` | `UdpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `UdpBackend` | SC | HAZ-004, HAZ-005 |
| `close()` | `UdpBackend` | NSC | — |
| `is_open()` | `UdpBackend` | NSC | — |
| `get_transport_stats()` | `UdpBackend` | NSC | — |
| `process_hello_or_validate()` | `UdpBackend` | SC | HAZ-015 |

`register_local_id()` and `send_hello_datagram()` reclassified SC (HAZ-005) as of the HELLO lifecycle fix (REQ-6.1.10): `register_local_id()` stores the local NodeId and calls `send_hello_datagram()` to transmit a HELLO frame to the peer; failure means the peer never registers this side's NodeId and all subsequent DATA frames are silently dropped (data-before-HELLO enforcement). This mirrors the TCP/TLS client pattern.

### src/platform/LocalSimHarness.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `LocalSimHarness` | NSC | — |
| `link()` | `LocalSimHarness` | NSC | — |
| `inject()` | `LocalSimHarness` | SC | HAZ-006 |
| `send_message()` | `LocalSimHarness` | SC | HAZ-001, HAZ-003, HAZ-005, HAZ-006 |
| `receive_message()` | `LocalSimHarness` | SC | HAZ-001, HAZ-004, HAZ-005 |
| `close()` | `LocalSimHarness` | NSC | — |
| `get_transport_stats()` | `LocalSimHarness` | NSC | — |

Note: `LocalSimHarness` implements `TransportInterface` and is used as the transport backend in integration tests that exercise the full SC send/receive path. Its `send_message()` and `receive_message()` carry the same message-delivery hazards as `TcpBackend`. The previous all-NSC classification was incorrect.

### src/core/TlsConfig.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `tls_config_default()` | — | NSC | — |

### src/platform/TlsSessionStore.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `TlsSessionStore()` (constructor) | `TlsSessionStore` | NSC | — |
| `~TlsSessionStore()` (destructor) | `TlsSessionStore` | NSC | — |
| `zeroize()` | `TlsSessionStore` | SC | HAZ-012, HAZ-017, HAZ-021 |
| `try_save()` | `TlsSessionStore` | SC | HAZ-017, HAZ-021 |
| `try_load()` | `TlsSessionStore` | SC | HAZ-017, HAZ-021 |

`zeroize()` is SC (HAZ-012, HAZ-017): it is the primary mechanism for clearing TLS session master-secret material from memory before the struct goes out of scope or is reused. Failure to call it (or compiler elision of a plain `memset`) leaves key-derived material readable from freed memory (CWE-316, CWE-14). `zeroize()` uses `mbedtls_platform_zeroize()` — which includes a compiler barrier — and calls `mbedtls_ssl_session_free()` before zeroing to release any internal mbedTLS allocations first (see SECURITY_ASSUMPTIONS.md §13). The destructor calls `zeroize()` as a safety net but is itself NSC: the hazard is only active if the caller relies solely on the destructor (i.e., does not call `zeroize()` explicitly before the struct goes out of scope); the destructor's invocation is automatic and not on the safety-critical message-delivery path. The constructor is NSC: it initialises the struct to a known-safe state; no security material is present at construction time.

### src/platform/TlsTcpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TlsTcpBackend` | SC | HAZ-020, HAZ-025 |
| `send_message()` | `TlsTcpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `TlsTcpBackend` | SC | HAZ-004, HAZ-005 |
| `register_local_id()` | `TlsTcpBackend` | NSC | — |
| `send_hello_frame()` | `TlsTcpBackend` | NSC | — |
| `find_client_slot()` | `TlsTcpBackend` | NSC | — |
| `handle_hello_frame()` | `TlsTcpBackend` | SC | HAZ-009 |
| `recv_from_client()` | `TlsTcpBackend` | SC | HAZ-009, HAZ-023 |
| `send_to_slot()` | `TlsTcpBackend` | NSC | — |
| `validate_source_id()` | `TlsTcpBackend` | SC | HAZ-009 |
| `classify_inbound_frame()` | `TlsTcpBackend` | SC | HAZ-009 |
| `tls_connect_handshake()` | `TlsTcpBackend` | SC | HAZ-008, HAZ-020, HAZ-025 |
| `evict_hello_timeout_slots()` | `TlsTcpBackend` | SC | HAZ-023 |
| `try_save_client_session()` | `TlsTcpBackend` | SC | HAZ-012, HAZ-017 |
| `try_load_client_session()` | `TlsTcpBackend` | SC | HAZ-017 |
| `set_session_store()` | `TlsTcpBackend` | NSC | — |
| `has_resumable_session()` | `TlsTcpBackend` | NSC | — |
| `setup_session_tickets()` | `TlsTcpBackend` | NSC | — |
| `maybe_setup_session_tickets()` | `TlsTcpBackend` | NSC | — |
| `apply_cipher_policy()` | `TlsTcpBackend` | NSC | — |
| `log_fs_warning_if_tls12()` | `TlsTcpBackend` | NSC | — |
| `close()` | `TlsTcpBackend` | NSC | — |
| `is_open()` | `TlsTcpBackend` | NSC | — |
| `get_transport_stats()` | `TlsTcpBackend` | NSC | — |

`TlsTcpBackend` is a drop-in replacement for `TcpBackend` (REQ-6.3.4). Its `send_message()` and `receive_message()` carry the same message-delivery hazards as `TcpBackend`. The TLS layer (when enabled) is an init-phase concern (cert/key loading, handshake) and does not alter the SC classification of the send/receive path. `classify_inbound_frame()` is SC (HAZ-009) because it enforces the one-HELLO-per-slot rule and rejects data from unregistered slots — failure here allows NodeId hijack or pre-registration data injection. **`handle_hello_frame()` reclassified SC (HAZ-009) as of SEC-012 (commit 4cda101):** it now performs a cross-slot scan for duplicate NodeId and evicts the newcomer; failure to scan allows a second connection to hijack an active peer's routing table entry. **`tls_connect_handshake()` reclassified SC (HAZ-008) as of SEC-021 (commit 4cda101):** it enforces `verify_peer=true` + non-empty `peer_hostname` before the TLS handshake, closing CWE-297 for the TLS TCP client path. **`try_save_client_session()` added SC (HAZ-012, HAZ-017):** saves TLS session material into the caller-injected `TlsSessionStore`; must call `store.zeroize()` first to release prior session allocations (HAZ-012) and sets `session_valid` atomically only on successful save, preventing partial/corrupt state from being loaded on reconnect (HAZ-017 — caller's cleanup responsibility begins once `session_valid == true`). **`try_load_client_session()` added SC (HAZ-017):** presents the saved session to mbedTLS for abbreviated resumption; if `session_valid` is true but the session data is stale (e.g., ticket expired server-side), the handshake falls back to a full exchange — the SC concern is that a successful load of compromised session material would bypass the full forward-secrecy handshake (HAZ-017 / RFC 5077 limitation). `set_session_store()` and `has_resumable_session()` are NSC: `set_session_store()` is called once during lifecycle configuration before `init()` and carries no runtime delivery policy; `has_resumable_session()` is a pure boolean predicate with no side effects. `setup_session_tickets()`, `maybe_setup_session_tickets()`, and `apply_cipher_policy()` are NSC: all three are called exclusively during `init()` to configure cipher suite policy and session ticket keying; they carry no runtime message-delivery policy. `log_fs_warning_if_tls12()` is NSC: a logging helper that emits a forward-secrecy advisory warning when the negotiated TLS version is 1.2; it has no effect on safety-relevant state and does not influence any delivery or authentication decision.

### src/platform/DtlsUdpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DtlsUdpBackend` | SC | HAZ-020, HAZ-025 |
| `register_local_id()` | `DtlsUdpBackend` | SC | HAZ-005 |
| `send_hello_datagram()` | `DtlsUdpBackend` | SC | HAZ-005 |
| `send_message()` | `DtlsUdpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `DtlsUdpBackend` | SC | HAZ-004, HAZ-005 |
| `client_connect_and_handshake()` | `DtlsUdpBackend` | SC | HAZ-008, HAZ-020, HAZ-025 |
| `deserialize_and_dispatch()` | `DtlsUdpBackend` | SC | HAZ-005 |
| `process_hello_or_validate()` | `DtlsUdpBackend` | SC | HAZ-009, HAZ-011 |
| `load_crl_if_configured()` | `DtlsUdpBackend` | NSC | — |
| `close()` | `DtlsUdpBackend` | NSC | — |
| `is_open()` | `DtlsUdpBackend` | NSC | — |
| `get_transport_stats()` | `DtlsUdpBackend` | NSC | — |

`DtlsUdpBackend` is a drop-in replacement for `UdpBackend` (REQ-6.3.4). Its `send_message()` and `receive_message()` carry the same message-delivery hazards as `UdpBackend`. The DTLS layer (when enabled) is an init-phase concern (cert/key loading, DTLS handshake, cookie exchange); enabling or disabling TLS does not alter the SC classification of the send/receive path. The MTU enforcement in `send_message()` (returns `ERR_INVALID` for payloads exceeding `DTLS_MAX_DATAGRAM_BYTES`) is part of the HAZ-005 mitigation. `deserialize_and_dispatch()` is SC (HAZ-005) as the inbound wire-to-envelope path — an error here could deliver a malformed envelope to the DeliveryEngine. `process_hello_or_validate()` is SC (HAZ-009, HAZ-011) because it enforces DTLS peer NodeId binding; failure allows source_id spoofing to corrupt ACK/retry state (HAZ-009 = TCP/TLS cross-transport class; HAZ-011 = DTLS-specific source_id spoofing after MAC verification). `register_local_id()` and `send_hello_datagram()` reclassified SC (HAZ-005) as of the HELLO lifecycle fix (REQ-6.1.10): in client mode `register_local_id()` sends the mandatory HELLO datagram via `send_hello_datagram()`; failure prevents the server from registering this side's NodeId, causing all subsequent DATA frames to be silently dropped (data-before-HELLO enforcement). Server mode returns OK immediately (no connected peer at registration time). `load_crl_if_configured()` is NSC: called only during `init()` certificate loading and carries no runtime message-delivery policy.

### src/platform/IMbedtlsOps.hpp / src/platform/MbedtlsOpsImpl.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `instance()` | `MbedtlsOpsImpl` | NSC | — |
| `psa_crypto_init()` | `MbedtlsOpsImpl` | NSC | — |
| `x509_crt_parse_file()` | `MbedtlsOpsImpl` | NSC | — |
| `pk_parse_keyfile()` | `MbedtlsOpsImpl` | NSC | — |
| `ssl_conf_own_cert()` | `MbedtlsOpsImpl` | NSC | — |
| `ssl_cookie_setup()` | `MbedtlsOpsImpl` | NSC | — |
| `ssl_setup()` | `MbedtlsOpsImpl` | NSC | — |
| `ssl_set_client_transport_id()` | `MbedtlsOpsImpl` | NSC | — |
| `ssl_set_hostname()` | `MbedtlsOpsImpl` | SC | HAZ-008 |
| `ssl_handshake()` | `MbedtlsOpsImpl` | SC | HAZ-004, HAZ-005, HAZ-006 |
| `ssl_write()` | `MbedtlsOpsImpl` | SC | HAZ-005, HAZ-006 |
| `ssl_read()` | `MbedtlsOpsImpl` | SC | HAZ-004, HAZ-005 |
| `recvfrom_peek()` | `MbedtlsOpsImpl` | SC | HAZ-004, HAZ-005 |
| `net_connect()` | `MbedtlsOpsImpl` | NSC | — |
| `inet_pton_ipv4()` | `MbedtlsOpsImpl` | NSC | — |

`MbedtlsOpsImpl` implements `IMbedtlsOps` and is injected into `DtlsUdpBackend` via its `init()` constructor parameter. `ssl_handshake()`, `ssl_write()`, `ssl_read()`, and `recvfrom_peek()` are SC: `ssl_handshake()` establishes the DTLS session — failure to complete the handshake (or silent success without proper certificate validation) directly enables HAZ-005 (data delivered without encryption integrity) and HAZ-004/HAZ-006 (connection not established → stale/dropped delivery); `ssl_write()` and `ssl_read()` are on the run-time send/receive path; `recvfrom_peek()` determines the peer address for DTLS cookie binding. `ssl_set_hostname()` is SC (HAZ-008): it binds the expected certificate CN/SAN before the DTLS handshake; omitting this call means CA-chain validation passes but hostname is unbound, constituting incomplete peer verification (CWE-297 / MitM — REQ-6.4.6). All other methods are called exclusively during `init()` (certificate loading, cookie setup, session configuration) and are NSC. `MbedtlsOpsImpl` is a pure delegation layer — each method is a precondition assertion plus one library call passthrough; no message-delivery policy is encoded here.

### src/core/AssertState.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `assert_state::set_reset_handler()` | — | NSC | — |
| `assert_state::get_reset_handler()` | — | NSC | — |
| `assert_state::trigger_handler_for_test()` | — | NSC | — |
| `assert_state::check_and_clear_fatal()` | — | NSC | — |
| `assert_state::get_fatal_count()` | — | NSC | — |

`get_fatal_count()` and `g_fatal_count` are REQ-7.2.4 observability hooks. They read the process-wide fatal assertion counter; they have no effect on delivery semantics and are classified NSC.

### src/core/DeliveryStats.hpp

All functions in this header are **NSC** — they are plain zero-initialisation helpers for the stats structs (`delivery_stats_init()`, `impairment_stats_init()`, `retry_stats_init()`, `ack_tracker_stats_init()`, `transport_stats_init()`). No message-delivery policy is encoded here.

### src/core/RequestReplyEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `RequestReplyEngine` | SC | HAZ-001, HAZ-002 |
| `send_request()` | `RequestReplyEngine` | SC | HAZ-001, HAZ-002 |
| `receive_request()` | `RequestReplyEngine` | SC | HAZ-001, HAZ-003 |
| `send_response()` | `RequestReplyEngine` | SC | HAZ-001, HAZ-002 |
| `receive_response()` | `RequestReplyEngine` | SC | HAZ-001, HAZ-002 |
| `sweep_timeouts()` | `RequestReplyEngine` | SC | HAZ-001, HAZ-002 |
| `receive_non_rr()` | `RequestReplyEngine` | NSC | — |

`RequestReplyEngine` provides bounded request/response correlation on top of `DeliveryEngine` (REQ-3.2.4). The six public SC functions are classified because: `send_request()` and `receive_response()` / `sweep_timeouts()` manage pending-request slots — exhaustion of those slots (HAZ-002: resource saturation) or a missed timeout sweep directly blocks message delivery (HAZ-001: wrong-node or failed delivery). `receive_request()` deduplicates inbound requests via `pump_inbound()` which calls `DeliveryEngine::receive()` — it sits on the HAZ-003 (duplicate delivery) path. `send_response()` and `init()` affect the same HAZ-001/HAZ-002 hazard set. `receive_non_rr()` is NSC: it is a passthrough stash accessor that retrieves non-RR DATA envelopes and has no reliability or delivery-decision policy. All private helpers (`pump_inbound`, `dispatch_inbound_envelope`, `handle_inbound_response`, `handle_inbound_request`, `find_pending`, `find_free_pending`, `build_wire_payload`, `parse_rr_header`, `stash_non_rr_data`) are private implementation details; their SC/NSC status is determined by the public function that calls them.

### HAZ-010, HAZ-012, HAZ-014, HAZ-017 — init-phase and teardown mitigations (no SC annotation required)

The following hazards are mitigated exclusively in init-phase initialisation or object teardown code, not in runtime SC functions. No `// Safety-critical (SC): HAZ-NNN` annotation is required on the mitigating code; the classification note below explains why each is NSC. Note: HAZ-017 is also mitigated by `TlsSessionStore::zeroize()` (SC — see `src/platform/TlsSessionStore.hpp` table above); the destructor safety-net entry here is the additional teardown-only path.

| HAZ ID | Mitigating code | Why NSC |
|--------|-----------------|---------|
| HAZ-010 | `DeliveryEngine::init()` — derives entropy seed for `MessageIdGen` via `arc4random_buf` / `getrandom` XOR `local_id` (DEF-015-2) | Init-phase only; runs once before any message is sent or received. The seed is consumed during `MessageIdGen::init()`; all subsequent PRNG calls (which are SC) use the already-seeded state. |
| HAZ-012 | `TlsTcpBackend::~TlsTcpBackend()` and `DtlsUdpBackend::~DtlsUdpBackend()` — call `mbedtls_platform_zeroize()` before `mbedtls_pk_free()` (DEF-016-2/3) | Teardown/destructor; executes after the transport is closed and no further message traffic can occur. Key zeroing is a residual-data concern, not a runtime message-delivery concern. |
| HAZ-014 | `OrderingBuffer::seq_next_guarded()` private helper — wraps UINT32_MAX+1 to 1, preventing the UNORDERED sentinel collision (DEF-016-5) | `seq_next_guarded()` is called only from `sweep_expired_holds()` and `advance_next_expected()`, both of which are classified NSC (sequence advancement is a lifecycle bookkeeping function; the SC delivery decision is in `ingest()` and `try_release_next()`). |
| HAZ-017 | `TlsSessionStore::~TlsSessionStore()` — calls `zeroize()` as a destructor safety net; `TlsTcpBackend::close()` logs `WARNING_HI` when `store.session_valid == true` after close | Destructor safety-net path only. The primary HAZ-017 mitigation is `TlsSessionStore::zeroize()` (SC — see classification table above); the destructor is a last-resort backstop that triggers automatically if the caller does not invoke `zeroize()` explicitly. It executes after `TlsTcpBackend` is closed, not on the active message-delivery path. |

---

### HAZ-018 through HAZ-025 — SC classification rationale

| HAZ ID | Newly-SC function(s) | Rationale |
|--------|----------------------|-----------|
| HAZ-018 | `AckTracker::on_ack()`, `AckTracker::get_tracked_destination()`, `DuplicateFilter::is_duplicate()`, `DuplicateFilter::check_and_record()`, `OrderingBuffer::ingest()` | Timing-oracle attack vector: variable-time equality comparison in these functions allows an attacker to distinguish partial-match from full-match via response latency, enabling ACK forgery and replay detection bypass. Constant-time comparator (`ct_bytes_equal()`) required per REQ-3.2.11. |
| HAZ-019 | `Serializer::deserialize()`, `ReassemblyBuffer::ingest()` | Wire-supplied length overflow: `frame_len` and `total_payload_length` near UINT32_MAX bypass bounds checks; ceiling guard required per REQ-3.2.10 before any arithmetic. |
| HAZ-020 | `TlsTcpBackend::init()`, `DtlsUdpBackend::init()`, `TlsTcpBackend::tls_connect_handshake()`, `DtlsUdpBackend::client_connect_and_handshake()` | Empty CA with `verify_peer=true` makes certificate verification a no-op; fail-fast required per REQ-6.3.6. Reclassified from NSC because `init()` now makes a security enforcement decision. |
| HAZ-021 | `TlsSessionStore::zeroize()`, `TlsSessionStore::try_save()`, `TlsSessionStore::try_load()` | Concurrent access race on TLS session struct; POSIX mutex required per REQ-6.3.10. `try_save` and `try_load` added as new SC functions. |
| HAZ-022 | `DeliveryEngine::init()` | Entropy source failure without fail-fast guard enables predictable `MessageIdGen` seeding and ACK forgery (extends HAZ-010); FATAL + reset required per REQ-5.2.6. Reclassified from NSC. |
| HAZ-023 | `TcpBackend::init()`, `TcpBackend::recv_from_client()`, `TcpBackend::evict_hello_timeout_slots()`, `TlsTcpBackend::init()`, `TlsTcpBackend::recv_from_client()`, `TlsTcpBackend::evict_hello_timeout_slots()` | Slow-connect slot exhaustion DoS; per-slot HELLO timeout required per REQ-6.1.12. `evict_hello_timeout_slots()` is a new SC function in both TCP backends. |
| HAZ-024 | `UdpBackend::init()` | Wildcard `peer_ip` enables NodeId hijack; init must reject wildcard per REQ-6.2.5. Reclassified from NSC. |
| HAZ-025 | `TlsTcpBackend::init()`, `DtlsUdpBackend::init()`, `TlsTcpBackend::tls_connect_handshake()`, `DtlsUdpBackend::client_connect_and_handshake()` | Silent MitM trap when `verify_peer=false` and `peer_hostname` is non-empty; fail-fast required per REQ-6.3.9. |

---

*End of HAZARD_ANALYSIS.md — update this document whenever a new HAZ entry, FMEA row, or SC classification change is required per CLAUDE.md §11.*
