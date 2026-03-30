# Software Safety Hazard Analysis, FMEA, and SC Function Classification

**Project:** messageEngine
**Standard:** NASA-STD-8719.13C (Software Safety), NASA-STD-8739.8A (Software Assurance)
**Policy reference:** CLAUDE.md §11
**Severity scale:** Cat I = Catastrophic, Cat II = Critical, Cat III = Marginal, Cat IV = Negligible

---

## §1 Software Safety Hazard Analysis

| ID | Hazard | Severity | Triggering Condition | Detection Mechanism | Mitigation |
|----|--------|----------|----------------------|---------------------|------------|
| HAZ-001 | **Wrong-node delivery** — a message is delivered to a node other than the intended destination | Cat II | `destination_id` corrupted in envelope; routing logic bug; Serializer endian error on destination field | `envelope_valid()` pre-check on every send and receive; `destination_id` validated by Serializer field-range check | `envelope_valid()` enforced before any `send()`; Serializer validates and round-trips `destination_id`; receiver checks `destination_id` matches local node |
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

### AckTracker

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `track()` returns `ERR_FULL` and caller ignores | Message never ACK-tracked; retry unlimited | Cat II HAZ-002, HAZ-006 | `ERR_FULL` return | Rule 7 requires return check; `WARNING_HI` logged |
| `on_ack()` returns `ERR_INVALID` (ACK for unknown ID) | Retry continues unnecessarily | Cat III HAZ-002 | `ERR_INVALID` return; `WARNING_LO` log | Retry eventually expires; does not cause incorrect delivery |
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

### TcpBackend / UdpBackend

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| Partial TCP frame accepted as complete message | Corrupt fields delivered | Cat I HAZ-005 | `tcp_recv_frame` uses `socket_recv_exact` | `socket_recv_exact` loops until all bytes received or error |
| `send_message()` called with no connected client | Message silently discarded | Cat III HAZ-006 | `m_client_count == 0` check; `WARNING_LO` logged | Logged; upper layer should verify connectivity before sending |
| `receive_message()` returns `ERR_TIMEOUT` spuriously | Upper layer delays message processing | Cat IV | Bounded `poll_count` loop | Caller retries; `RECV_TIMEOUT_MS` tunable per channel |

### ImpairmentEngine

| Failure Mode | System Effect | Severity | Detection | Mitigation |
|---|---|---|---|---|
| `process_outbound()` skips partition check | Partition not simulated; real link loss masked | Cat III HAZ-007 | `is_partition_active()` called first in `process_outbound()` | First check in function; cannot be bypassed |
| `check_loss()` uses uninitialized PRNG | Non-deterministic / always-pass behavior | Cat III HAZ-007 | `NEVER_COMPILED_OUT_ASSERT(m_initialized)` | `init()` must be called; assertion fires if not |
| Delay buffer full; message silently dropped | HAZ-006 | Cat II | `ERR_FULL` returned; `WARNING_HI` logged | `m_delay_count < IMPAIR_DELAY_BUF_SIZE` checked before `queue_to_delay_buf()` |
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
| `sweep_expired()` | `AckTracker` | SC | HAZ-002, HAZ-006 |

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

### src/core/DeliveryEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `DeliveryEngine` | NSC | — |
| `send()` | `DeliveryEngine` | SC | HAZ-001, HAZ-002, HAZ-006 |
| `receive()` | `DeliveryEngine` | SC | HAZ-001, HAZ-003, HAZ-004, HAZ-005 |
| `pump_retries()` | `DeliveryEngine` | SC | HAZ-002 |
| `sweep_ack_timeouts()` | `DeliveryEngine` | SC | HAZ-002, HAZ-006 |

### src/core/TransportInterface.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TransportInterface` | NSC | — |
| `send_message()` | `TransportInterface` | SC | HAZ-001, HAZ-005, HAZ-006 |
| `receive_message()` | `TransportInterface` | SC | HAZ-001, HAZ-004, HAZ-005 |
| `close()` | `TransportInterface` | NSC | — |
| `is_open()` | `TransportInterface` | NSC | — |

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

### src/platform/ImpairmentEngine.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `ImpairmentEngine` | NSC | — |
| `process_outbound()` | `ImpairmentEngine` | SC | HAZ-002, HAZ-007 |
| `collect_deliverable()` | `ImpairmentEngine` | SC | HAZ-004, HAZ-006 |
| `process_inbound()` | `ImpairmentEngine` | SC | HAZ-003, HAZ-007 |
| `is_partition_active()` | `ImpairmentEngine` | SC | HAZ-007 |
| `config()` | `ImpairmentEngine` | NSC | — |

### src/platform/SocketUtils.hpp

All `SocketUtils` functions are **NSC** — raw POSIX I/O primitives with no message-delivery policy. Failure returns are propagated to callers; no silent discard occurs at this layer.

### src/platform/TcpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `TcpBackend` | NSC | — |
| `send_message()` | `TcpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `TcpBackend` | SC | HAZ-004, HAZ-005 |
| `close()` | `TcpBackend` | NSC | — |
| `is_open()` | `TcpBackend` | NSC | — |

### src/platform/UdpBackend.hpp

| Function | Class | SC/NSC | HAZ IDs |
|---|---|---|---|
| `init()` | `UdpBackend` | NSC | — |
| `send_message()` | `UdpBackend` | SC | HAZ-005, HAZ-006 |
| `receive_message()` | `UdpBackend` | SC | HAZ-004, HAZ-005 |
| `close()` | `UdpBackend` | NSC | — |
| `is_open()` | `UdpBackend` | NSC | — |

### src/platform/LocalSimHarness.hpp

All `LocalSimHarness` methods are **NSC** — test harness only; not used in production deployment.

---

*End of HAZARD_ANALYSIS.md — update this document whenever a new HAZ entry, FMEA row, or SC classification change is required per CLAUDE.md §11.*
