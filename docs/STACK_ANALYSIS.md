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

## Resolution of DEF-031-1 — flush-path stack allocation (2026-04-15)

**DEF-031-1 resolved.** All five transport backends (`TcpBackend`, `TlsTcpBackend`,
`DtlsUdpBackend`, `UdpBackend`, `LocalSimHarness`) previously declared a stack-local
array in each flush helper:

```cpp
MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];  // was 32 × 4,144 = 132,608 bytes (~130 KB)
```

**Fix applied:** `delayed[]` replaced with a pre-allocated private member
`MessageEnvelope m_delay_buf[IMPAIR_DELAY_BUF_SIZE] = {}` in each class. The fix matches
the existing `m_retry_buf` / `m_timeout_buf` pattern in `DeliveryEngine`.

**Test infrastructure change (required by fix):** Adding the 132 KB member raises each
backend struct from ~540 KB to ~674 KB. Stack-allocating a ~674 KB struct in test thread
functions caused SIGSEGV on macOS ARM64 + LLVM coverage (lazy stack-page-mapping). All
test functions that previously declared backends as stack-local variables now heap-allocate
them (`new`/`delete`); this is permitted by the test relaxations in CLAUDE.md §1b
(dynamic allocation in test fixture setup and teardown).

**Stack impact:**
- Flush-path frame (`flush_delayed_to_clients()` / `flush_delayed_to_wire()`): ~130 KB → ~48 B
- Chain 3 (retry pump, full path): ~130 KB → ~592 B
- New worst case by stack size: Chain 5 (DTLS outbound) at ~764 B (~1,326 B with Logger)

**Embedded porting note:** With this fix applied, all backends now store `m_delay_buf` as
a pre-allocated member. Applications targeting embedded platforms must heap-allocate or
statically declare backend objects (not stack-local) to avoid ~674 KB stack frames during
object construction. Validation with a stack-analysis tool (avstack, StackAnalyzer) is
still recommended before deployment on any target with < 1 MB stack headroom.

---

## Worst-Case Call Chains

Seven independent worst-case paths are identified: send, receive, retry, ACK sweep, DTLS
outbound, DTLS inbound, and HELLO registration. All flush helpers now use `m_delay_buf`
(a pre-allocated member) rather than a stack-local array (DEF-031-1 resolved).
`DtlsUdpBackend::send_message()` calls `flush_delayed_to_wire()` from `receive_message()`,
not from `send_message()`; the two call paths are therefore never simultaneously live.

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
                           └─ TcpBackend::drain_readable_clients()  [~32 B]
                                └─ TcpBackend::recv_from_client()  [~48 B]
                                     └─ Serializer::deserialize()  [~32 B]
```

**Depth:** 9 frames
**Estimated peak stack:** ~480 B

---

### Chain 3 — Retry pump

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::pump_retries()  [~64 B]  ← m_retry_buf is a pre-allocated member array; stack frame is loop variables only
                 └─ RetryManager::collect_due()  [~48 B]
                      └─ DeliveryEngine::send_fragments()  [~48 B]
                           └─ DeliveryEngine::send_via_transport()  [~48 B]
                                └─ TcpBackend::send_message()  [~48 B]
                                     └─ TcpBackend::flush_delayed_to_clients()  [~48 B]  ← m_delay_buf is now a pre-allocated member (DEF-031-1 resolved)
                                          └─ TcpBackend::send_one_delayed()  [~48 B]
                                               └─ Serializer::serialize()  [~32 B]
```

**Depth:** 11 frames  ← **worst-case frame depth across all chains**
**Estimated peak stack:** ~592 B

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

Note: `flush_delayed_to_wire()` uses `m_delay_buf` (a pre-allocated member, DEF-031-1 resolved)
and is called only from `DtlsUdpBackend::receive_message()`, not from `send_message()`.
The two call paths are therefore never simultaneously live.

**`ssl_handshake` note:** `run_dtls_handshake()` (called during `init()`, not at runtime) also
dispatches through `IMbedtlsOps::ssl_handshake()` → `MbedtlsOpsImpl::ssl_handshake()` for
up to `DTLS_HANDSHAKE_MAX_ITER` = 32 iterations. Because this occurs entirely within `init()`
and not on any send/receive runtime path, it does not appear in any of the chains above
and does not change the worst-case runtime stack depth.

**Depth:** 9 frames
**Estimated peak stack (our code, non-flush path):** ~764 B

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
is 5 frames deep, well below the 11-frame worst case of Chain 3. It does **not** represent
a new worst-case path.

`TlsTcpBackend::register_local_id()` and `TlsTcpBackend::send_hello_frame()` have an
identical chain structure; the only difference is that the final send call goes through
`mbedtls_ssl_write()` rather than a plain socket write, adding at most 1 extra frame
(the mbedTLS internal call). Maximum depth: 6 frames. Still not a new worst case.

---

### Chain 6 — DTLS inbound receive

`recv_one_dtls_datagram()` calls `try_tls_recv()` and `deserialize_and_dispatch()` as
sequential branches, not nested calls. The diagram below reflects this:

```
main()  [~64 B]
  └─ run_client_iteration()  [~80 B]
       └─ wait_for_echo()  [~64 B]
            └─ DeliveryEngine::receive()  [~80 B]
                 └─ DtlsUdpBackend::receive_message()  [~80 B]
                      └─ DtlsUdpBackend::recv_one_dtls_datagram()  [~48 B]
                           ├─ DtlsUdpBackend::try_tls_recv()  [~32 B]   ← sequential branch A
                           │    └─ MbedtlsOpsImpl::ssl_read()  [~16 B]  ← IMbedtlsOps virtual dispatch
                           └─ DtlsUdpBackend::deserialize_and_dispatch()  [~48 B]  ← sequential branch B
                                └─ Serializer::deserialize()  [~32 B]
```

The maximum nesting depth reached by either branch is 8 frames (branch A or branch B each
add 2 frames below `recv_one_dtls_datagram()`; they are not nested within each other).

**Depth:** 8 frames
**Estimated peak stack:** ~568 B

---

## Summary

| Chain | Depth (frames) | Peak stack estimate |
|-------|---------------|---------------------|
| 1 — Outbound send (TCP/UDP) | 9 | ~748 B |
| 2 — Inbound receive (TCP/UDP) | 9 | ~480 B |
| 3 — Retry pump (with flush) | **11** | ~592 B |
| 4 — ACK timeout sweep | 6 | ~352 B |
| 5 — DTLS outbound send | 9 | **~764 B** (~1,326 B with Logger) |
| 6 — DTLS inbound receive | 8 | ~568 B |
| 7 — HELLO registration (init-phase only) | 5–6 | ~256 B |
| **Worst case (frame depth)** | **11 (Chain 3 — retry pump with send_fragments)** | ~592 B |
| **Worst case (stack size)** | **~764 B (Chain 5 — DTLS outbound; ~1,326 B with Logger)** | |

The worst-case **frame depth** is **11 frames** (Chain 3 — retry pump with `send_fragments()`).
The worst-case **stack size** is **~764 B** (Chain 5, DTLS outbound; ~1,326 B including the
Logger frame). Chain 3 is now ~592 B after DEF-031-1 was resolved.

DEF-002-1 resolved (2026-04-02): `m_retry_buf` and `m_timeout_buf` moved from stack-local
arrays to private member arrays in `DeliveryEngine`, zero-initialized in `init()`.
DEF-031-1 resolved (2026-04-15): `delayed[]` in flush helpers replaced with member
`m_delay_buf` in all five backends; tests heap-allocate backends. Flush-path worst case:
~130 KB → ~592 B (Chain 3). Chain 5 (DTLS outbound, ~764 B) is now the stack-size worst case.

---

## Platform Stack Budget

| Platform | Default per-thread stack | Headroom vs. worst case (~764 B, Chain 5) |
|----------|--------------------------|-------------------------------------------|
| macOS | 8 MB (main), 512 KB (pthreads) | >10 000× |
| Linux | 8 MB (main), 8 MB (pthreads) | >10 000× |

No stack overflow risk exists on macOS/Linux at the default stack sizes. The flush-path
worst case is now ~592 B (Chain 3, after DEF-031-1 resolved); the overall worst case by
stack size is Chain 5 (DTLS outbound) at ~764 B (~1,326 B with Logger). Both are well
within any reasonable stack budget.

**Embedded porting note:** Backends now store `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` as a
pre-allocated member (~132 KB per backend struct). Applications targeting constrained
platforms must heap-allocate or statically declare backend objects rather than
stack-declaring them to avoid ~674 KB stack frames during object construction.
Validation with a stack-analysis tool (avstack, StackAnalyzer) is recommended for
any target with < 1 MB stack headroom.

---

## Logger call chain (NSC-infrastructure — added 2026-04-14)

`Logger::log()` and `Logger::log_wall()` are called from every LOG_* macro site across the
codebase. Because they own a 512-byte stack buffer, their call chain is documented here per
the update trigger (stack-allocated buffer > 256 bytes).

```
<any caller>  [varies]
  └─ Logger::log() [buf[512] + ~50 B locals]
       ├─ s_clock->now_monotonic_us()  [PosixLogClock::now_monotonic_us(): ~16 B]
       │    └─ PosixLogClock::do_clock_gettime()  [~16 B]  ← NVI seam, thin POSIX wrapper
       └─ s_sink->write()  [PosixLogSink::write(): ~16 B]
            └─ PosixLogSink::do_write()  [~16 B]  ← NVI seam, wraps ::write(2)
```

**Depth added to any call chain containing a LOG_* macro:** 4 frames (Logger::log +
`now_monotonic_us` + `do_clock_gettime` + `write` + `do_write` — 5 frames from the call
site, but the call site itself is already counted in its parent chain).

**Stack allocation:** `Logger::log()` owns `char buf[LOG_FORMAT_BUF_SIZE]` = `buf[512]` plus
approximately 50 bytes of local variables (timestamp decomposition, snprintf return values,
computed write_len). Total for the Logger frame: ~562 bytes. The remaining three frames each
contribute ~16 bytes. Total added by the Logger chain: ~610 bytes.

**Worst-case impact:** The 512-byte buffer in Logger::log() exceeds the 256-byte threshold
that triggers an update to this document. However, it does **not** displace any existing
worst-case chain because:

1. **Frame depth:** Adding 4 frames to Chain 3 (the depth worst case at 11 frames) would
   produce 15 frames — but Chain 3 calls `flush_delayed_to_clients()` which itself calls
   `send_one_delayed()` → `Serializer::serialize()`. These inner callers already contain
   LOG_* macros; the 4-frame Logger chain is already implicitly present in Chain 3's leaf
   calls. No new worst-case depth is introduced.

2. **Stack size:** After DEF-031-1 was resolved, the flush-path worst case (Chain 3) is ~592 B.
   The Logger frame (~562 B) raises Chain 3 to at most ~1,154 B when a LOG_* macro fires
   within the flush path — still well below any platform stack limit. Chain 5 (~764 B) rises
   to ~1,326 B with Logger, which remains the size worst case.

3. **Stack-size worst case (Chain 5):** The worst case by stack size is Chain 5 at ~764 B.
   A call to `Logger::log()` on that path adds ~562 B, raising the estimate to ~1,326 B —
   still well within the platform headroom of ≥ 8 MB (macOS/Linux).

**Updated worst-case estimate:** ~1,326 B (Chain 5 + Logger; previously the dominant concern
was the ~130 KB flush path, which is now resolved). All platform headroom figures remain valid.

The chain structure is documented here for completeness per the stack-analysis update
policy. No entry in the Summary table requires revision; the floor and ceiling estimates
are unchanged.

---

## Update Trigger

This document must be updated when:
- A new function is added that creates a stack-allocated buffer > 256 bytes.
- `IMPAIR_DELAY_BUF_SIZE` or `sizeof(MessageEnvelope)` changes (affects `m_delay_buf` size and Chain 3/5 estimates).
- A new call chain adds frames beyond depth 11.
- A new thread entry point is added.
- A new thread entry point is added (start a new chain analysis from that entry point).

Frame size estimates are conservative upper bounds; actual sizes depend on compiler
optimisation and ABI. On embedded targets with ≤ 64 KB stack, re-derive estimates
using a stack-analysis tool (e.g., avstack, StackAnalyzer) or linker map inspection.
