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
```

**Depth:** 7 frames
**Estimated peak stack:** ~448 B

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

## Summary

| Chain | Depth (frames) | Peak stack estimate |
|-------|---------------|---------------------|
| 1 — Outbound send | 9 | ~748 B |
| 2 — Inbound receive | 8 | ~480 B |
| 3 — Retry pump | 7 | ~448 B |
| 4 — ACK timeout sweep | 6 | ~384 B |
| **Worst case** | **9** | **~748 B** |

The worst-case estimated stack usage is approximately **748 bytes** across 9 frames.
This is dominated by the `payload_buf[256]` local array in `send_test_message()`.

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
- A new call chain adds frames beyond depth 9.
- A new thread entry point is added (start a new chain analysis from that entry point).

Frame size estimates are conservative upper bounds; actual sizes depend on compiler
optimisation and ABI. On embedded targets with ≤ 64 KB stack, re-derive estimates
using a stack-analysis tool (e.g., avstack, StackAnalyzer) or linker map inspection.
