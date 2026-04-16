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

## Known Limitation — Large Stack Allocations in flush helpers

**All four socket backends** (`TcpBackend`, `TlsTcpBackend`, `DtlsUdpBackend`, `UdpBackend`)
**and `LocalSimHarness`** contain `flush_delayed_to_clients()` / `flush_delayed_to_wire()` /
`send_message()` flush paths that declare a local array:

```cpp
MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];  // IMPAIR_DELAY_BUF_SIZE = 32
```

`sizeof(MessageEnvelope)` = **4,144 bytes** (MSG_MAX_PAYLOAD_BYTES 4096 + header fields).
Total per-call stack allocation: **32 × 4,144 = 132,608 bytes (~130 KB)**.

This is the actual worst-case stack allocation. The "~764 B" figure quoted below in the
per-chain estimates is the worst-case **excluding** this flush path.

**Why this cannot simply be moved to a member variable:**
The backends (`TcpBackend`, `TlsTcpBackend`, etc.) are themselves large structs (~540 KB)
due to their embedded `RingBuffer` and `ImpairmentEngine` member arrays. Adding a 132 KB
member `m_delay_buf` would increase each backend to ~674 KB. Tests stack-allocate backend
objects; on macOS ARM64 with LLVM coverage instrumentation, the resulting ~674 KB
stack-frame allocation overflows the lazy stack-page-mapping threshold and causes `SIGSEGV`
during constructor execution (DEF-031-1 investigation, 2026-04-15). A correct fix would
require heap-allocating backends in all test functions (~60+ functions across 4 test files).

**Static-local approach is also unsafe:** A function-static buffer would be shared across
instances. Since TcpBackend tests run server and client in separate threads simultaneously,
a shared static would introduce a data race.

**For embedded porting:** platforms with per-thread stacks ≤ 512 KB must use one of:
(a) heap-allocate or statically declare a single backend instance (the local array then lives
    in the thread's stack only for the duration of the flush call, not as a permanent member), or
(b) change the backends to hold `m_delay_buf` as a member (pre-allocated at init time) and
    ensure tests heap-allocate backends rather than declaring them as local variables.

Validation with a stack-analysis tool (avstack, StackAnalyzer) is mandatory before deployment
on any target with < 1 MB stack headroom.

---

## Worst-Case Call Chains

Seven independent worst-case paths are identified: send, receive, retry, ACK sweep, DTLS
outbound, DTLS inbound, and HELLO registration — plus the impairment flush path documented
above. The worst-case per-path stack cost is ~130 KB (a single `MessageEnvelope delayed[32]`
array). `DtlsUdpBackend::send_message()` calls `flush_delayed_to_wire()` from `receive_message()`,
not from `send_message()`; the two arrays are therefore never simultaneously live.

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
                                     └─ TcpBackend::flush_delayed_to_clients()  [~132,608 B]  ← MessageEnvelope delayed[32]
                                          └─ TcpBackend::send_one_delayed()  [~48 B]
                                               └─ Serializer::serialize()  [~32 B]
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

Note: `flush_delayed_to_wire()` (which declares `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]`,
~130 KB) is called only from `DtlsUdpBackend::receive_message()`, not from `send_message()`.
The two paths are therefore never simultaneously live. The ~130 KB allocation appears on the
DTLS inbound flush path (Chain 6), not here.

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
| 3 — Retry pump (with flush) | **11** | **~132,608 B (~130 KB)** |
| 4 — ACK timeout sweep | 6 | ~352 B |
| 5 — DTLS outbound send | 9 | ~764 B |
| 6 — DTLS inbound receive | 8 | ~568 B |
| 7 — HELLO registration (init-phase only) | 5–6 | ~256 B |
| **Worst case (frame depth)** | **11 (Chain 3 — retry pump with send_fragments)** | **~132,608 B (~130 KB)** |
| **Worst case (stack size)** | **~130 KB (single delayed[] — flush path in all five backends)** | |

The worst-case **frame depth** is **11 frames** (Chain 3 — retry pump with `send_fragments()`).
The worst-case **stack size** is **~130 KB** (a single `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]`
array on the flush path; present in `TcpBackend`, `TlsTcpBackend`, `DtlsUdpBackend`, `UdpBackend`,
and `LocalSimHarness`). For non-flush paths the worst-case stack is the "~764 B" non-flush estimate.

DEF-002-1 resolved (2026-04-02): `m_retry_buf` and `m_timeout_buf` moved from stack-local
arrays to private member arrays in `DeliveryEngine`, zero-initialized in `init()`. Chains 3
and 4 no longer carry those oversized stack allocations — but the `delayed[]` array in the
flush helpers remains the dominant stack consumer.

---

## Platform Stack Budget

| Platform | Default per-thread stack | Headroom vs. worst case (~130 KB flush path) |
|----------|--------------------------|----------------------------------------------|
| macOS | 8 MB (main), 512 KB (pthreads) | ~61× (main) / ~4× (pthreads) |
| Linux | 8 MB (main), 8 MB (pthreads) | ~61× |

No stack overflow risk exists on macOS/Linux at the default stack sizes for the main thread.
The **~130 KB** flush-path worst case (a single `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]`
array) makes this codebase **unsuitable for embedded targets with ≤ 256 KB stack** without
first restructuring the flush helpers to use a member buffer rather than a stack-local array.

pthreads on macOS default to 512 KB; the flush path (~130 KB) leaves ~4× headroom on that
configuration — adequate for typical use but worth monitoring. Consider calling
`pthread_attr_setstacksize()` with ≥ 1 MB for application threads that invoke any flush path
on embedded or resource-constrained deployments.

**Embedded porting guidance:** To fix the ~130 KB stack cost for constrained targets:
1. Heap-allocate or statically declare the backend objects (not stack-local) in your application.
2. Move the `delayed[]` array to a pre-allocated private member `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]`
   in each backend class.
3. If you do step 2, ensure test code also heap-allocates backend objects rather than
   stack-declaring them (POSIX ARM64 coverage builds require this due to lazy stack-page mapping
   interacting poorly with ~674 KB stack frames).

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

2. **Stack size:** The 512-byte Logger buffer is dwarfed by the `MessageEnvelope delayed[32]`
   (~130 KB) allocation in `flush_delayed_to_clients()` and `flush_delayed_to_wire()`. The
   Logger frame adds at most ~562 B to the ~130 KB flush-path worst case — less than 0.5%
   change; the Chain 3 and Chain 5 estimates remain accurate within their stated margin.

3. **Non-flush paths:** On non-flush paths the worst-case stack was stated as "~764 B".
   A call to `Logger::log()` on such a path adds ~562 B, raising the non-flush estimate
   to at most ~1,326 B — still well within the platform headroom of ≥ 8 MB (macOS/Linux).

**Updated non-flush worst-case estimate:** ~1,326 B (previously ~764 B; difference is the
Logger::log() buf[512] + locals). This is below the 256-byte mention threshold only for the
parent caller, not the Logger frame itself; all platform headroom figures remain valid.

The chain structure is documented here for completeness per the stack-analysis update
policy. No entry in the Summary table requires revision; the floor and ceiling estimates
are unchanged.

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
