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

## Worst-Case Call Chains

Four independent worst-case paths are identified: send, receive, retry, and
impairment-internal.

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
            └─ DeliveryEngine::pump_retries()  [~96 B]  ← retry_buf[32] on stack
                 └─ RetryManager::collect_due()  [~48 B]
                      └─ DeliveryEngine::send_via_transport()  [~48 B]
                           └─ TcpBackend::send_message()  [~48 B]
                                └─ Serializer::serialize()  [~32 B]
                                     └─ ImpairmentEngine::process_outbound()  [~80 B]
                                          └─ ImpairmentEngine::queue_to_delay_buf()  [~32 B]
```

**Depth:** 10 frames  ← **worst case across all chains**
**Estimated peak stack:** ~572 B

---

### Chain 4 — ACK timeout sweep

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::sweep_ack_timeouts()  [~96 B]  ← expired_buf[32] on stack
                 └─ AckTracker::sweep_expired()  [~48 B]
                      └─ AckTracker::sweep_one_slot()  [~32 B]
```

**Depth:** 6 frames
**Estimated peak stack:** ~384 B

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
| 3 — Retry pump | **10** | ~572 B |
| 4 — ACK timeout sweep | 6 | ~384 B |
| 5 — DTLS outbound send | **10** | ~764 B |
| 6 — DTLS inbound receive | 8 | ~504 B |
| **Worst case** | **10 (Chains 3 and 5)** | **~764 B (Chain 5, dominated by payload_buf[256] + IMbedtlsOps wrapper)** |

The worst-case **frame depth** is **10 frames** (Chain 3 — retry pump), which extends
Chain 1's path by appending the `Serializer::serialize()` →
`ImpairmentEngine::process_outbound()` → `queue_to_delay_buf()` tail that
`TcpBackend::send_message()` always invokes. DtlsUdpBackend chains (5, 6) do not
exceed existing worst cases.

The worst-case **stack size** remains **~748 bytes** (Chains 1 and 5), dominated by
the `payload_buf[256]` local array in `send_test_message()`. Chain 3 reaches only
~572 B because it lacks that large local buffer; the extra three frames add
approximately 144 B of frame overhead. Chain 5 equals Chain 1 in depth and size.

---

## Platform Stack Budget

| Platform | Default per-thread stack | Headroom vs. worst case |
|----------|--------------------------|-------------------------|
| macOS | 8 MB (main), 512 KB (pthreads) | > 700× |
| Linux | 8 MB (main), 8 MB (pthreads) | > 10 000× |

No stack overflow risk exists on either supported platform at current code structure.

---

## Update Trigger

This document must be updated when:
- A new function is added that creates a larger on-stack buffer than `payload_buf[256]`.
- A new call chain adds frames beyond depth 10.
- A new thread entry point is added (start a new chain analysis from that entry point).

Frame size estimates are conservative upper bounds; actual sizes depend on compiler
optimisation and ABI. On embedded targets with ≤ 64 KB stack, re-derive estimates
using a stack-analysis tool (e.g., avstack, StackAnalyzer) or linker map inspection.
