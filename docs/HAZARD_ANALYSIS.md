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

### DuplicateFilter

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `check_and_record()` false negative (misses duplicate) | Duplicate delivered and acted on | Cat II HAZ-003 | Window-based FIFO with `(source_id, message_id)` pairs | `DEDUP_WINDOW_SIZE` ≥ `max_retries`; uniqueness guaranteed by `MessageIdGen::next()` |
| `check_and_record()` false positive (drops unique) | Valid message silently lost | Cat III | Message ID monotonicity | Monotone IDs; window slides forward only; no wraparound in flight window |
| `init()` not called before use | Undefined state; assertions fire | Cat II | `NEVER_COMPILED_OUT_ASSERT(m_initialized)` | Called in `DeliveryEngine::init()` before any message processing |

### Serializer

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `serialize()` buffer overflow | Truncated or corrupt wire frame sent | Cat I HAZ-005 | Bounds check before every write; returns `ERR_OVERFLOW` | Fixed-layout serialization; `SOCKET_RECV_BUF_BYTES` sized for max envelope |
| `deserialize()` reads beyond buffer | Corrupt fields reconstructed silently | Cat I HAZ-005 | `offset + sizeof(field) > len` guard on every read | Returns `ERR_INVALID` on any out-of-bounds read |
| Endian conversion error | Field interpreted with wrong byte order | Cat I HAZ-005 | Fixed big-endian encoding; deterministic layout | `write_u32` / `read_u32` always swap; no host-endian assumptions |
| Payload length mismatch | Truncated payload delivered | Cat I HAZ-005 | `payload_length` field vs. actual bytes copied | `payload_length <= MSG_MAX_PAYLOAD_BYTES` enforced in both directions |
| Protocol version or magic mismatch silently accepted | Frame from incompatible wire format decoded with wrong field layout; corrupt data delivered to caller | Cat I HAZ-005 | `proto_ver != PROTO_VERSION` check at byte 3; `magic_word != (PROTO_MAGIC << 16)` check at bytes 40–43 in `deserialize()` | `ERR_INVALID` returned immediately on version or magic mismatch before any field is read; logged at `WARNING_HI` (REQ-3.2.8) |

### TcpBackend / UdpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| Partial TCP frame accepted as complete message | Corrupt fields delivered | Cat I HAZ-005 | `tcp_recv_frame` uses `socket_recv_exact` | `socket_recv_exact` loops until all bytes received or error |
| `send_message()` called with no connected client | Message silently discarded | Cat III HAZ-006 | `m_client_count == 0` check; `WARNING_LO` logged | Logged; upper layer should verify connectivity before sending |
| `receive_message()` returns `ERR_TIMEOUT` spuriously | Upper layer delays message processing | Cat IV | Bounded `poll_count` loop | Caller retries; `RECV_TIMEOUT_MS` tunable per channel |

### TlsTcpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| TLS handshake failure (bad cert/key) | Connection not established; no messages exchanged | Cat III | `mbedtls_ssl_handshake()` returns non-zero; logged at `WARNING_HI`; `init()` returns `ERR_IO` | Certificate and key loaded and validated in `setup_tls_config()` before bind/connect; `ERR_IO` forces caller to abort |
| TLS cert/key files missing or corrupt | `init()` fails; transport not opened | Cat III | `mbedtls_x509_crt_parse_file()` / `mbedtls_pk_parse_keyfile()` return non-zero; `ERR_IO` returned | `is_open()` returns `false`; upper layer must check `init()` result |
| Plaintext mode used when TLS expected | Unencrypted traffic on secured link | Cat II | `tls_enabled` flag controls code path | Configuration-level control; operator responsibility to set `tls_enabled=true` when security is required |
| Partial TLS record delivered | Corrupt fields reconstructed silently | Cat I HAZ-005 | `mbedtls_ssl_read()` returns exact byte count; 4-byte length prefix checked | `tls_recv_frame()` validates payload length against header; rejects oversized frames |

### DtlsUdpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| DTLS handshake failure (bad cert/key) | Connection not established; no messages exchanged | Cat III | `mbedtls_ssl_handshake()` returns non-zero; logged at `WARNING_HI`; `init()` returns `ERR_IO` | Cert/key validated in `setup_dtls_config()` before handshake; `ERR_IO` forces caller to abort |
| DTLS cert/key files missing or corrupt | `init()` fails; transport not opened | Cat III | `mbedtls_x509_crt_parse_file()` / `mbedtls_pk_parse_keyfile()` return non-zero | `is_open()` returns `false`; upper layer must check `init()` result |
| Plaintext mode used when DTLS expected | Unencrypted traffic on secured link | Cat II | `tls_enabled` flag; if `false`, plaintext UDP path taken (REQ-6.4.5) | Operator responsibility to set `tls_enabled=true`; same pattern as TlsTcpBackend |
| DTLS cookie exchange bypass | Amplification attack vector; unverified client | Cat II | `mbedtls_ssl_conf_dtls_cookies()` armed in server `setup_dtls_config()`; cookie checked on every ServerHello | Cookie context initialised and armed before any handshake; `HELLO_VERIFY_REQUIRED` handled in `run_dtls_handshake()` (REQ-6.4.2) |
| Serialized payload exceeds DTLS MTU | IP fragmentation or send failure | Cat III HAZ-005 | `wire_len > DTLS_MAX_DATAGRAM_BYTES` check in `send_message()`; returns `ERR_INVALID` | Hard check before every send; `mbedtls_ssl_set_mtu()` configures DTLS record MTU (REQ-6.4.4) |
| DTLS handshake timeout (server never receives first datagram) | `init()` returns `ERR_TIMEOUT`; transport not opened | Cat III | `poll()` in `server_wait_and_handshake()` times out after `connect_timeout_ms` | `ERR_TIMEOUT` propagated to caller; upper layer must retry or abort |

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
| `deserialize()` | `Serializer` | SC | HAZ-001, HAZ-005 |

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
| `on_ack()` | `AckTracker` | SC | HAZ-002 |
| `cancel()` | `AckTracker` | SC | HAZ-002 |
| `sweep_expired()` | `AckTracker` | SC | HAZ-002, HAZ-006 |
| `get_send_timestamp()` | `AckTracker` | NSC | — |
| `get_stats()` | `AckTracker` | NSC | — |

### src/core/DuplicateFilter.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DuplicateFilter` | NSC | — |
| `is_duplicate()` | `DuplicateFilter` | SC | HAZ-003 |
| `record()` | `DuplicateFilter` | SC | HAZ-003 |
| `check_and_record()` | `DuplicateFilter` | SC | HAZ-003 |

### src/core/RetryManager.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `RetryManager` | NSC | — |
| `schedule()` | `RetryManager` | SC | HAZ-002 |
| `on_ack()` | `RetryManager` | SC | HAZ-002 |
| `collect_due()` | `RetryManager` | SC | HAZ-002 |
| `get_stats()` | `RetryManager` | NSC | — |

### src/core/Fragmentation.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `needs_fragmentation()` | — | NSC | — |
| `fragment_message()` | — | SC | HAZ-001, HAZ-006 |

### src/core/ReassemblyBuffer.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `ReassemblyBuffer` | NSC | — |
| `ingest()` | `ReassemblyBuffer` | SC | HAZ-003, HAZ-006 |
| `sweep_expired()` | `ReassemblyBuffer` | NSC | — |

### src/core/OrderingBuffer.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `OrderingBuffer` | NSC | — |
| `ingest()` | `OrderingBuffer` | SC | HAZ-001, HAZ-006 |
| `try_release_next()` | `OrderingBuffer` | SC | HAZ-001 |
| `advance_sequence()` | `OrderingBuffer` | NSC | — |

### src/core/DeliveryEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DeliveryEngine` | NSC | — |
| `send()` | `DeliveryEngine` | SC | HAZ-001, HAZ-002, HAZ-006 |
| `receive()` | `DeliveryEngine` | SC | HAZ-001, HAZ-003, HAZ-004, HAZ-005 |
| `pump_retries()` | `DeliveryEngine` | SC | HAZ-002 |
| `sweep_ack_timeouts()` | `DeliveryEngine` | SC | HAZ-002, HAZ-006 |
| `get_stats()` | `DeliveryEngine` | NSC | — |

### src/core/TransportInterface.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TransportInterface` | NSC | — |
| `send_message()` | `TransportInterface` | SC | HAZ-001, HAZ-005, HAZ-006 |
| `receive_message()` | `TransportInterface` | SC | HAZ-001, HAZ-004, HAZ-005 |
| `register_local_id()` | `TransportInterface` | NSC | — |
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
| `next_range()` | `PrngEngine` | SC | HAZ-002, HAZ-007 |

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
| `init()` | `TcpBackend` | NSC | — |
| `send_message()` | `TcpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `TcpBackend` | SC | HAZ-004, HAZ-005 |
| `register_local_id()` | `TcpBackend` | NSC | — |
| `send_hello_frame()` | `TcpBackend` | NSC | — |
| `find_client_slot()` | `TcpBackend` | NSC | — |
| `handle_hello_frame()` | `TcpBackend` | NSC | — |
| `send_to_slot()` | `TcpBackend` | NSC | — |
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
| `init()` | `UdpBackend` | NSC | — |
| `send_message()` | `UdpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `UdpBackend` | SC | HAZ-004, HAZ-005 |
| `close()` | `UdpBackend` | NSC | — |
| `is_open()` | `UdpBackend` | NSC | — |
| `get_transport_stats()` | `UdpBackend` | NSC | — |

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

### src/platform/TlsTcpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TlsTcpBackend` | NSC | — |
| `send_message()` | `TlsTcpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `TlsTcpBackend` | SC | HAZ-004, HAZ-005 |
| `register_local_id()` | `TlsTcpBackend` | NSC | — |
| `send_hello_frame()` | `TlsTcpBackend` | NSC | — |
| `find_client_slot()` | `TlsTcpBackend` | NSC | — |
| `handle_hello_frame()` | `TlsTcpBackend` | NSC | — |
| `send_to_slot()` | `TlsTcpBackend` | NSC | — |
| `close()` | `TlsTcpBackend` | NSC | — |
| `is_open()` | `TlsTcpBackend` | NSC | — |
| `get_transport_stats()` | `TlsTcpBackend` | NSC | — |

`TlsTcpBackend` is a drop-in replacement for `TcpBackend` (REQ-6.3.4). Its `send_message()` and `receive_message()` carry the same message-delivery hazards as `TcpBackend`. The TLS layer (when enabled) is an init-phase concern (cert/key loading, handshake) and does not alter the SC classification of the send/receive path. The five new functions (`register_local_id()`, `send_hello_frame()`, `find_client_slot()`, `handle_hello_frame()`, `send_to_slot()`) are classified NSC for the same reasons as the equivalent `TcpBackend` functions — see the TcpBackend rationale note above.

### src/platform/DtlsUdpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DtlsUdpBackend` | NSC | — |
| `send_message()` | `DtlsUdpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `DtlsUdpBackend` | SC | HAZ-004, HAZ-005 |
| `close()` | `DtlsUdpBackend` | NSC | — |
| `is_open()` | `DtlsUdpBackend` | NSC | — |
| `get_transport_stats()` | `DtlsUdpBackend` | NSC | — |

`DtlsUdpBackend` is a drop-in replacement for `UdpBackend` (REQ-6.3.4). Its `send_message()` and `receive_message()` carry the same message-delivery hazards as `UdpBackend`. The DTLS layer (when enabled) is an init-phase concern (cert/key loading, DTLS handshake, cookie exchange); enabling or disabling TLS does not alter the SC classification of the send/receive path. The MTU enforcement in `send_message()` (returns `ERR_INVALID` for payloads exceeding `DTLS_MAX_DATAGRAM_BYTES`) is part of the HAZ-005 mitigation.

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
| `ssl_handshake()` | `MbedtlsOpsImpl` | SC | HAZ-004, HAZ-005, HAZ-006 |
| `ssl_write()` | `MbedtlsOpsImpl` | SC | HAZ-005, HAZ-006 |
| `ssl_read()` | `MbedtlsOpsImpl` | SC | HAZ-004, HAZ-005 |
| `recvfrom_peek()` | `MbedtlsOpsImpl` | SC | HAZ-004, HAZ-005 |
| `net_connect()` | `MbedtlsOpsImpl` | NSC | — |
| `inet_pton_ipv4()` | `MbedtlsOpsImpl` | NSC | — |

`MbedtlsOpsImpl` implements `IMbedtlsOps` and is injected into `DtlsUdpBackend` via its `init()` constructor parameter. `ssl_handshake()`, `ssl_write()`, `ssl_read()`, and `recvfrom_peek()` are SC: `ssl_handshake()` establishes the DTLS session — failure to complete the handshake (or silent success without proper certificate validation) directly enables HAZ-005 (data delivered without encryption integrity) and HAZ-004/HAZ-006 (connection not established → stale/dropped delivery); `ssl_write()` and `ssl_read()` are on the run-time send/receive path; `recvfrom_peek()` determines the peer address for DTLS cookie binding. All other methods are called exclusively during `init()` (certificate loading, cookie setup, session configuration) and are NSC. `MbedtlsOpsImpl` is a pure delegation layer — each method is a precondition assertion plus one library call passthrough; no message-delivery policy is encoded here.

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

---

*End of HAZARD_ANALYSIS.md — update this document whenever a new HAZ entry, FMEA row, or SC classification change is required per CLAUDE.md §11.*
