# Worst-Case Execution Time (WCET) Analysis

**Project:** messageEngine
**Standard:** NASA-STD-8719.13C §4 (timing analysis for safety-critical software)
**Policy reference:** CLAUDE.md §14
**Platform note:** This is a PC-hosted library running on macOS / Linux with
non-deterministic OS scheduling. Classical WCET measurement (requiring deterministic
hardware and WCET tools such as aiT or Rapita) is not applicable here. Instead,
this document provides **static worst-case operation counts** — the maximum number
of operations any SC function can perform as a closed-form expression of the
compile-time capacity constants defined in src/core/Types.hpp. On a deterministic
embedded target, these counts can be multiplied by the per-operation clock cycles
to derive a true WCET bound.

---

## Capacity Constants (from src/core/Types.hpp)

| Constant | Value | Role |
|----------|-------|------|
| `MSG_RING_CAPACITY` | 64 | RingBuffer depth |
| `ACK_TRACKER_CAPACITY` | 32 | Max outstanding unACKed messages |
| `DEDUP_WINDOW_SIZE` | 128 | DuplicateFilter sliding window |
| `MAX_RETRY_COUNT` | 5 | Max retries per message |
| `IMPAIR_DELAY_BUF_SIZE` | 32 | ImpairmentEngine delay/reorder buffer |
| `MAX_TCP_CONNECTIONS` | 8 | TcpBackend simultaneous client count |
| `MSG_MAX_PAYLOAD_BYTES` | 4096 | Maximum payload size in bytes |
| `SOCKET_RECV_BUF_BYTES` | 8192 | Wire buffer size |

Let **R** = `MSG_RING_CAPACITY` = 64,
    **A** = `ACK_TRACKER_CAPACITY` = 32,
    **D** = `DEDUP_WINDOW_SIZE` = 128,
    **I** = `IMPAIR_DELAY_BUF_SIZE` = 32,
    **C** = `MAX_TCP_CONNECTIONS` = 8,
    **P** = `MSG_MAX_PAYLOAD_BYTES` = 4096.

---

## SC Function WCET Table

### src/core/MessageEnvelope.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `envelope_init()` | 1 memset of sizeof(MessageEnvelope) | O(P) = O(4096) | Dominated by payload zero-fill |
| `envelope_copy()` | 1 memcpy of sizeof(MessageEnvelope) | O(P) = O(4096) | Dominated by payload copy |
| `envelope_valid()` | 1 comparison | O(1) | |
| `envelope_is_data()` | 1 comparison | O(1) | |
| `envelope_is_control()` | 1 comparison | O(1) | |
| `envelope_make_ack()` | Field assignments + 1 memset | O(1) | No payload copy |

### src/core/MessageId.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `MessageIdGen::init()` | 1 assignment | O(1) | |
| `MessageIdGen::next()` | 1 increment + 1 conditional | O(1) | |

### src/core/Timestamp.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `timestamp_now_us()` | 1 clock_gettime syscall + 1 overflow guard comparison (SEC-009) | O(1) | SEC-009 adds 1 comparison before the multiply: `if (tv_sec > UINT64_MAX/1000000ULL) return UINT64_MAX`. CERT INT30-C. No change to asymptotic bound. |
| `timestamp_expired()` | 2 comparisons | O(1) | |
| `timestamp_deadline_us()` | 1 addition + 1 comparison | O(1) | |

### src/core/Serializer.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `Serializer::serialize()` | Header writes (fixed) + P payload bytes | O(P) = O(4096) | Dominated by memcpy of payload |
| `Serializer::deserialize()` | Header reads (fixed) + 2 additional comparisons (proto_version, magic_word) + P payload bytes | O(P) = O(4096) | 2 extra comparisons are O(1); bound unchanged; dominated by memcpy of payload |

### src/core/RingBuffer.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `RingBuffer::push()` | 1 envelope_copy + 2 index ops | O(P) = O(4096) | Dominated by envelope_copy payload |
| `RingBuffer::pop()` | 1 envelope_copy + 2 index ops | O(P) = O(4096) | Dominated by envelope_copy payload |

### src/core/AckTracker.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `AckTracker::track()` | Linear scan for FREE slot | O(A) = O(32) | |
| `AckTracker::on_ack()` | Linear scan for matching PENDING | O(A) = O(32) | |
| `AckTracker::sweep_expired()` | Full scan of all slots | O(A) = O(32) | Calls sweep_one_slot per slot |

### src/core/DuplicateFilter.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `DuplicateFilter::is_duplicate()` | Linear scan of window | O(D) = O(128) | **Highest-complexity dedup path** |
| `DuplicateFilter::record()` | 1 write + 1 index advance | O(1) | |
| `DuplicateFilter::check_and_record()` | is_duplicate + record | O(D) = O(128) | |

### src/core/RetryManager.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `RetryManager::schedule()` | Linear scan for free slot | O(A) = O(32) | Uses ACK_TRACKER_CAPACITY slots |
| `RetryManager::on_ack()` | Linear scan for matching entry | O(A) = O(32) | |
| `RetryManager::collect_due()` | Full scan; envelope_copy per due entry | O(A × P) = O(32 × 4096) | **Worst case: 32 due entries each copied** |

### src/core/DeliveryEngine.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `DeliveryEngine::send()` | 1 monotonic guard (SEC-007) + serialize + process_outbound + track + schedule | O(P + I + A) | SEC-007 adds O(1) guard at entry: `if (now_us < m_last_now_us) return ERR_INVALID`. Dominated by serialize O(P). |
| `DeliveryEngine::receive()` | 1 monotonic guard (SEC-007) + drain_hello_reconnects (O(N)) + receive_message + is_duplicate + check_and_record | O(P + D + N) | SEC-007 adds O(1) guard at entry. `drain_hello_reconnects()` adds O(N) = O(ORDERING_PEER_COUNT=16) per call (bounded poll loop). Dominated by deserialize O(P) and dedup O(D). |
| `DeliveryEngine::pump_retries()` | 1 monotonic guard (SEC-007) + collect_due (O(A)) + per-due: send_via_transport → serialize (O(P)) + process_outbound + collect_deliverable (O(I)) | O(A × (P + I)) = O(32 × 4128) = O(132096) | **Worst case: all 32 slots due simultaneously; each retry traverses Serializer + ImpairmentEngine.** SEC-007 adds O(1) guard at entry. DEF-002-1 resolved: output buffer is now member `m_retry_buf` (init-phase allocated). |
| `DeliveryEngine::sweep_ack_timeouts()` | 1 monotonic guard (SEC-007) + sweep_expired O(A) | O(A) = O(32) | SEC-007 adds O(1) guard at entry. DEF-002-1 resolved: output buffer is now member `m_timeout_buf` (init-phase allocated). |
| `DeliveryEngine::reset_peer_ordering()` | 1 held_pending check (O(1)) + `reset_peer()` (O(N + H)) | O(N + H) = O(16 + 8) = O(24) | N = ORDERING_PEER_COUNT=16 (peer table scan in find_peer); H = ORDERING_HOLD_COUNT=8 (hold slot scan). SC: HAZ-001, HAZ-016. Called per HELLO event from drain_hello_reconnects(). |

### src/core/TransportInterface.hpp (concrete implementations)

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `TcpBackend::send_message()` | serialize (O(P)) + process_outbound + collect_deliverable (O(I)) + send to C clients (O(P × C)) | O(P × C + I × P) = O(4096 × 8 + 32 × 4096) = O(163840) | Wire write to each client; when all I delay-buffer slots are simultaneously due, collect_deliverable flushes them all — each requiring a separate wire write of O(P) |
| `TcpBackend::receive_message()` | poll_count × (accept + C recv + deserialize) | O(poll_count × C × P) | poll_count ≤ 50; bound = O(50 × 8 × 4096) |
| `TcpBackend::register_local_id()` | 1 NodeId store + send_hello_frame() (serialize O(P) + 1 send syscall) | O(P) = O(4096) | O(1) bookkeeping + one HELLO wire write. Called once at init time; not on any runtime send/receive path. NSC. |
| `TcpBackend::find_client_slot()` | Linear scan of m_client_node_ids[] | O(C) = O(8) | Called by flush_delayed_to_clients() for unicast routing; adds O(C) per delayed message on top of existing O(I) outer loop. NSC. |
| `TcpBackend::flush_delayed_to_clients()` (updated) | collect_deliverable (O(I)) + per-delayed-msg: serialize O(P) + find_client_slot O(C) + send O(P) | O(I × (P + C)) = O(32 × (4096 + 8)) = O(131 328) | Previously O(I × P); unicast routing adds find_client_slot() O(C)=O(8) per delayed message. Bound unchanged at O(I × P) asymptotically; constant factor increase is negligible. SC path via send_message() (HAZ-005, HAZ-006). |
| `UdpBackend::send_message()` | serialize + process_outbound + 1 sendto | O(P) = O(4096) | Single datagram |
| `UdpBackend::receive_message()` | poll_count × (recv + deserialize) | O(poll_count × P) = O(50 × 4096) | |

### src/core/Fragmentation.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `Fragmentation::fragment_message()` | Up to FRAG_MAX_COUNT (4) envelope copies + header field writes | O(FRAG_MAX_COUNT × P) = O(4 × 4096) | SC: HAZ-001, HAZ-006. Each fragment carries a copy of the payload slice; dominant cost is FRAG_MAX_COUNT × `envelope_copy()`. Cost is implicit in `send_fragments()` called from each retry-pump iteration. |
| `Fragmentation::needs_fragmentation()` | 1 comparison | O(1) | NSC. |

### src/platform/ImpairmentEngine.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `ImpairmentEngine::process_outbound()` | partition check + loss check + queue_to_delay_buf | O(I) = O(32) | Linear scan for free slot |
| `ImpairmentEngine::collect_deliverable()` | Full scan of delay buffer | O(I) = O(32) | |
| `ImpairmentEngine::process_inbound()` | Full scan of reorder buffer | O(I) = O(32) | |
| `ImpairmentEngine::is_partition_active()` | 2 comparisons | O(1) | |

### src/platform/ImpairmentConfigLoader.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `impairment_config_load()` | fopen + MAX_CONFIG_LINES × fgets + parse_config_line + fclose | O(MAX_CONFIG_LINES) = O(64) | **Init-phase only.** Each `fgets` reads at most `MAX_CONFIG_LINE_LEN` = 128 bytes; each `parse_config_line` runs two `sscanf` calls (O(1)) and one `strcmp` chain (O(12) keys × O(key_len)). No heap allocation; fixed 128-byte stack buffer per line. |

### src/platform/TlsTcpBackend.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `send_message()` | serialize (O(P)) + process_outbound + collect_deliverable (O(I)) + TLS write to C clients (O(P × C)) | O(P × C + I × P) = O(163 840) | Identical bound to `TcpBackend::send_message()`; TLS record framing is O(P) per frame (mbedTLS adds ≤ 29-byte overhead per record). TLS handshake cost is init-phase only and not included here. |
| `receive_message()` | poll_count × (net_poll + accept + C TLS recv + deserialize) | O(poll_count × C × P) = O(1 638 400) | poll_count ≤ 50; `mbedtls_net_poll()` is O(1) (select on 1 fd). TLS decryption is O(P) per record. Bound equals `TcpBackend::receive_message()`. |
| `register_local_id()` | 1 NodeId store + send_hello_frame() (serialize O(P) + 1 TLS write O(P)) | O(P) = O(4096) | O(1) bookkeeping + one HELLO TLS record write. Called once at init time; not on any runtime path. NSC. |
| `find_client_slot()` | Linear scan of m_client_node_ids[] | O(C) = O(8) | Called by flush_delayed_to_clients() for unicast routing; adds O(C) per delayed message on top of existing O(I) outer loop. NSC. |
| `flush_delayed_to_clients()` (updated) | collect_deliverable (O(I)) + per-delayed-msg: serialize O(P) + find_client_slot O(C) + TLS write O(P) | O(I × (P + C)) = O(32 × (4096 + 8)) = O(131 328) | Unicast routing adds find_client_slot() O(C)=O(8) per delayed message. Asymptotic bound unchanged at O(I × P). SC path via send_message() (HAZ-005, HAZ-006). |

### src/platform/DtlsUdpBackend.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `send_message()` | serialize (O(P)) + MTU check (O(1)) + process_outbound (O(I)) + collect_deliverable (O(I)) + 1 vtable dispatch (O(1)) + mbedtls_ssl_write (O(P)) | O(P + I) = O(4096 + 32) | Single datagram (single-peer model). `IMbedtlsOps` virtual dispatch (1 pointer dereference + call) is O(1) and does not change the asymptotic bound. `ERR_INVALID` returned early for P > DTLS_MAX_DATAGRAM_BYTES = 1400; in practice P ≤ 1400 on the critical path. DTLS encryption is O(P) per record; handshake cost is init-phase only. |
| `receive_message()` | poll_count × (recv_one_dtls_datagram (O(P)) + 1 vtable dispatch (O(1))) + flush_delayed_to_queue (O(I)) | O(poll_count × P + I) = O(50 × 4096 + 32) | poll_count ≤ 50; each `IMbedtlsOps::ssl_read` virtual dispatch is O(1) and folds into the per-datagram cost. Bound equal to `UdpBackend::receive_message()`. |

### src/platform/IMbedtlsOps (interface) / MbedtlsOpsImpl

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `ssl_handshake()` | up to `DTLS_HANDSHAKE_MAX_ITER` = 32 vtable dispatches × (1 pointer deref + 1 `mbedtls_ssl_handshake` call) | O(32) = O(1) dispatches (init-phase only) | Called from `run_dtls_handshake()` during `init()` only; not on any runtime send/receive path. The mbedTLS handshake cost per call is non-deterministic (depends on network RTT); on a deterministic embedded target, use the library's documented per-state operation count. Does not contribute to runtime WCET. |
| `ssl_write()` | 1 vtable dispatch + 1 `mbedtls_ssl_write` call | O(P) dominated by mbedTLS record encryption | Wrapper overhead O(1); underlying cost already included in `DtlsUdpBackend::send_message()` row above. |
| `ssl_read()` | 1 vtable dispatch + 1 `mbedtls_ssl_read` call | O(P) dominated by mbedTLS record decryption | Wrapper overhead O(1); underlying cost already included in `DtlsUdpBackend::receive_message()` row above. |
| All other `MbedtlsOpsImpl` methods | 1 vtable dispatch + 1 library call passthrough | O(1) per method (wrapper overhead only) | All other methods are init-phase thin wrappers: one pointer dereference + one function call. Cost is not on any runtime SC path. |

### src/platform/PrngEngine.hpp

| Function | Worst-case operations | Bound | Notes |
|----------|----------------------|-------|-------|
| `PrngEngine::seed()` | 1 assignment | O(1) | |
| `PrngEngine::next()` | 4 arithmetic ops (xorshift64) | O(1) | |
| `PrngEngine::next_double()` | next() + 1 division | O(1) | |
| `PrngEngine::next_range()` | next() + 1 modulo | O(1) | |

---

## Dominant Cost Paths

The three highest-cost operations in the system:

1. **`DeliveryEngine::pump_retries()` with all slots due:** O(A × (P + I)) = O(32 × (4096 + 32)) = O(132 096 operations). Each retry slot invokes the full `TcpBackend::send_message()` path including `Serializer::serialize()` O(P) and `ImpairmentEngine::process_outbound()` + `collect_deliverable()` O(I). In practice, exponential backoff and `max_retries` prevent all slots from firing simultaneously.

2. **`TcpBackend::send_message()` with full delay buffer due:** O(P × C + I × P) = O(4096 × 8 + 32 × 4096) = O(163 840 operations). The `collect_deliverable()` flush after every send can release up to I=32 buffered messages, each requiring a wire write. This bound is loose; in practice, the delay buffer rarely reaches capacity.

3. **`TcpBackend::receive_message()` at maximum poll depth:** O(50 × 8 × 4096) = O(1 638 400 operations). This bound is loose; in practice, receive returns early on first message.

3. **`Serializer::serialize()` / `deserialize()`:** O(P) = O(4096) byte operations per call, which is the payload copy. Unavoidable for maximum-sized messages.

---

## Guidance for Embedded Porting

To derive a true WCET on a deterministic target:

1. Replace each `timestamp_now_us()` syscall with the target's hardware timer read latency.
2. Replace O(P) byte-copy bounds with `P × (cycles per byte copy)` for the target's bus width.
3. Replace O(A), O(D), O(I) loop bounds with the constant values above × (cycles per iteration body).
4. The worst-case call chain by **frame depth** is Chain 3 (retry pump) or Chain 5 (DTLS outbound) from STACK_ANALYSIS.md (10 frames each); sum the per-function WCETs along that chain. The worst-case **stack size** is ~764 B (Chain 5). DEF-002-1 resolved: output buffers in `pump_retries()` and `sweep_ack_timeouts()` are now init-phase member allocations.

---

## Update Trigger

This document must be updated when:
- Any capacity constant in src/core/Types.hpp changes.
- A new SC function is added (add a row to the appropriate table).
- A loop bound in an existing SC function changes.
