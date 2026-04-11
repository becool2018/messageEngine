# Worst-Case Stack Depth Analysis

**Project:** messageEngine
**Standard:** NASA-STD-8719.13C §4 (stack depth analysis for safety-critical software)
**Policy reference:** CLAUDE.md §13
**Platform:** POSIX (macOS / Linux); default per-thread stack ≥ 8 MB

---

## Basis

Power of 10 Rule 1 (no recursion) is enforced across the entire codebase. With no
recursion, the call graph is a directed acyclic graph (DAG). The worst-case stack
depth is therefore the length of the longest path in that DAG, which can be
enumerated statically without instrumentation.

---

## Critical Warning — Large Stack Allocations in flush_delayed_to_clients / flush_delayed_to_wire

**All four backends** (`TcpBackend`, `TlsTcpBackend`, `DtlsUdpBackend`, `UdpBackend`) contain
`flush_delayed_to_clients()` / `flush_delayed_to_wire()` helpers that declare a local array:

```cpp
MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];  // IMPAIR_DELAY_BUF_SIZE = 32
```

`sizeof(MessageEnvelope)` = **4,144 bytes** (MSG_MAX_PAYLOAD_BYTES 4096 + header fields).
Total per-call stack allocation: **32 × 4,144 = 132,608 bytes (~130 KB)**.

This is the actual worst-case stack allocation. The "~764 B" figure quoted below in the
per-chain estimates is the worst-case **excluding** this flush path.

**For embedded porting:** platforms with per-thread stacks ≤ 256 KB must either:
(a) restructure these helpers to use a single-element iteration rather than a local array, or
(b) allocate the delay buffer as a member array (pre-allocated at init time, not on the stack).
Validation with a stack-analysis tool (avstack, StackAnalyzer) is mandatory before deployment
on any target with < 1 MB stack headroom.

---

## Worst-Case Call Chains

Five independent worst-case paths are identified: send, receive, retry, ACK sweep, and
DTLS outbound — plus the impairment flush path documented above.

### Chain 1 — Outbound message send

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ send_test_message()  [~300 B]  ← large: payload_buf[256] on stack
            └─ DeliveryEngine::send()  [~64 B]
                 └─ DeliveryEngine::send_via_transport()  [~48 B]
                      └─ TcpBackend::send_message()  [~48 B]
                           └─ Serializer::serialize()  [~32 B]
                                └─ ImpairmentEngine::process_outbound()  [~80 B]
                                     └─ ImpairmentEngine::queue_to_delay_buf()  [~32 B]
```

**Depth:** 9 frames
**Estimated peak stack:** ~748 B

---

### Chain 2 — Inbound message receive

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::receive()  [~80 B]
                 └─ TcpBackend::receive_message()  [~80 B]
                      └─ TcpBackend::poll_clients_once()  [~32 B]
                           └─ TcpBackend::recv_from_client()  [~48 B]
                                └─ Serializer::deserialize()  [~32 B]
```

**Depth:** 8 frames
**Estimated peak stack:** ~480 B

---

### Chain 3 — Retry pump

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::pump_retries()  [~64 B]  ← m_retry_buf is a pre-allocated member array; stack frame is loop variables only
                 └─ RetryManager::collect_due()  [~48 B]
                      └─ DeliveryEngine::send_via_transport()  [~48 B]
                           └─ TcpBackend::send_message()  [~48 B]
                                └─ TcpBackend::flush_delayed_to_clients()  [~132,608 B]  ← MessageEnvelope delayed[32]
                                     └─ Serializer::serialize()  [~32 B]
                                          └─ ImpairmentEngine::process_outbound()  [~80 B]
                                               └─ ImpairmentEngine::queue_to_delay_buf()  [~32 B]
```

**Depth:** 11 frames  ← **worst-case frame depth across all chains**
**Estimated peak stack:** ~132,608 B (~130 KB — dominated by `delayed[IMPAIR_DELAY_BUF_SIZE]`)

---

### Chain 4 — ACK timeout sweep

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::sweep_ack_timeouts()  [~48 B]  ← m_timeout_buf is a pre-allocated member array; stack frame is loop variables only
                 └─ AckTracker::sweep_expired()  [~48 B]
                      └─ AckTracker::sweep_one_slot()  [~32 B]
```

**Depth:** 6 frames
**Estimated peak stack:** ~352 B

---

### Chain 5 — DTLS outbound send

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ send_test_message()  [~300 B]  ← large: payload_buf[256] on stack
            └─ DeliveryEngine::send()  [~64 B]
                 └─ DeliveryEngine::send_via_transport()  [~48 B]
                      └─ DtlsUdpBackend::send_message()  [~48 B]
                           └─ Serializer::serialize()  [~32 B]
                                └─ ImpairmentEngine::process_outbound()  [~80 B]
                                     └─ ImpairmentEngine::queue_to_delay_buf()  [~32 B]
```

When `flush_delayed_to_clients()` delivers the message via `IMbedtlsOps::ssl_write()`, one
additional frame is pushed (the `MbedtlsOpsImpl::ssl_write()` thin-wrapper, ~16 B) before
the library call. This increases the depth to 10 frames on the flush path but does not
change the peak stack estimate materially.

**`ssl_handshake` note:** `run_dtls_handshake()` (called during `init()`, not at runtime) also
dispatches through `IMbedtlsOps::ssl_handshake()` → `MbedtlsOpsImpl::ssl_handshake()` for
up to `DTLS_HANDSHAKE_MAX_ITER` = 32 iterations. Because this occurs entirely within `init()`
and not on any send/receive runtime path, it does not appear in any of the six chains above
and does not change the worst-case runtime stack depth.

**Depth:** 10 frames (with `MbedtlsOpsImpl::ssl_write()` virtual dispatch; 9 without)
**Estimated peak stack (our code):** ~764 B (adds ~16 B wrapper frame)

**mbedTLS library note:** `mbedtls_ssl_write()` invokes DTLS record-layer encryption internally. The mbedTLS call stack is not enumerable by static inspection of this codebase, but the init-phase deviation documented in DtlsUdpBackend.hpp confirms all allocations occur in `init()` and not on the send path. For embedded deployment, use a tool-based analysis (avstack or StackAnalyzer) that includes the mbedTLS library's object files.

---

### Chain 7 — HELLO registration (TcpBackend / TlsTcpBackend)

```
DeliveryEngine::init()  [~64 B]
  └─ TransportInterface::register_local_id()  [~32 B]  ← virtual dispatch
       └─ TcpBackend::register_local_id()  [~48 B]
            └─ send_hello_frame()  [~80 B]  ← m_wire_buf is pre-allocated member array
                 └─ Serializer::serialize()  [~32 B]
```

**Depth:** 5 frames
**Estimated peak stack:** ~256 B

This chain is called once during `init()`, not at runtime. `send_hello_frame()` uses the
pre-allocated `m_wire_buf` member array; no large stack allocation is introduced. The chain
is 5 frames deep, well below the 10-frame worst case of Chains 3 and 5. It does **not**
represent a new worst-case path.

`TlsTcpBackend::register_local_id()` and `TlsTcpBackend::send_hello_frame()` have an
identical chain structure; the only difference is that the final send call goes through
`mbedtls_ssl_write()` rather than a plain socket write, adding at most 1 extra frame
(the mbedTLS internal call). Maximum depth: 6 frames. Still not a new worst case.

---

### Chain 6 — DTLS inbound receive

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::receive()  [~80 B]
                 └─ DtlsUdpBackend::receive_message()  [~80 B]
                      └─ DtlsUdpBackend::recv_one_dtls_datagram()  [~48 B]
                           └─ MbedtlsOpsImpl::ssl_read()  [~16 B]  ← IMbedtlsOps virtual dispatch
                                └─ Serializer::deserialize()  [~32 B]
```

**Depth:** 8 frames (one extra for `MbedtlsOpsImpl::ssl_read()` thin wrapper)
**Estimated peak stack:** ~504 B

---

## Summary

| Chain | Depth (frames) | Peak stack estimate |
|-------|---------------|---------------------|
| 1 — Outbound send (TCP/UDP) | 9 | ~748 B |
| 2 — Inbound receive (TCP/UDP) | 8 | ~480 B |
| 3 — Retry pump (with flush) | **11** | **~132,608 B (~130 KB)** |
| 4 — ACK timeout sweep | 6 | ~352 B |
| 5 — DTLS outbound send (with flush) | **11** | **~132,624 B (~130 KB)** |
| 6 — DTLS inbound receive | 8 | ~504 B |
| 7 — HELLO registration (init-phase only) | 5–6 | ~256 B |
| **Worst case** | **11 (Chains 3 and 5)** | **~132,608 B (all backends with flush_delayed_to_clients / flush_delayed_to_wire)** |

The worst-case **frame depth** is **11 frames** (Chains 3 and 5 when impairment flush is
active). The worst-case **stack size** is **~130 KB**, dominated by
`MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` in `flush_delayed_to_clients()` /
`flush_delayed_to_wire()` across all four backends.

DEF-002-1 resolved (2026-04-02): `m_retry_buf` and `m_timeout_buf` moved from stack-local
arrays to private member arrays in `DeliveryEngine`, zero-initialized in `init()`. Chains 3
and 4 no longer carry those oversized stack allocations — but the `delayed[]` array in the
flush helpers remains the dominant stack consumer.

---

## Platform Stack Budget

| Platform | Default per-thread stack | Headroom vs. worst case (~130 KB) |
|----------|--------------------------|-----------------------------------|
| macOS | 8 MB (main), 512 KB (pthreads) | ~62× (main) / ~4× (pthreads) |
| Linux | 8 MB (main), 8 MB (pthreads) | ~62× |

No stack overflow risk exists on macOS/Linux at the default stack sizes. However, the
~130 KB worst-case makes this codebase **unsuitable for embedded targets with ≤ 256 KB
stack without first restructuring `flush_delayed_to_clients()` / `flush_delayed_to_wire()`
to iterate one envelope at a time rather than declaring a full `delayed[32]` local array.**

pthreads on macOS default to 512 KB; the flush path leaves only ~4× headroom on that
configuration — borderline for safe deployment with deep call stacks from the thread entry
point. Consider calling `pthread_attr_setstacksize()` with ≥ 2 MB for application threads
that invoke the message send path.

---

## Update Trigger

This document must be updated when:
- A new function is added that creates a larger on-stack buffer than `delayed[IMPAIR_DELAY_BUF_SIZE]` (~130 KB).
- `IMPAIR_DELAY_BUF_SIZE` or `sizeof(MessageEnvelope)` changes (both affect the flush-path worst case).
- `flush_delayed_to_clients()` / `flush_delayed_to_wire()` is restructured to use a member buffer (worst case drops).
- A new call chain adds frames beyond depth 11.
- A new thread entry point is added (start a new chain analysis from that entry point).

Frame size estimates are conservative upper bounds; actual sizes depend on compiler
optimisation and ABI. On embedded targets with ≤ 64 KB stack, re-derive estimates
using a stack-analysis tool (e.g., avstack, StackAnalyzer) or linker map inspection.
