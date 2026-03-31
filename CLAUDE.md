<application_requirements>
NOTE: Global high-level C/C++ coding standards (Power of 10, MISRA, F´ subset, architecture and layering rules) are defined in .claude/CLAUDE.md.
This section defines project-specific application requirements for the shared network, messaging, impairment engine, and transports.

1. Overall goals and scope
- Provide a shared network abstraction that:
  - Models realistic communication problems (latency, jitter, loss, duplication, reordering, partitions).
  - Offers consistent, reusable messaging utilities for all components.
  - Supports multiple transports (TCP, UDP, and a local simulation harness).
- All components must be designed for:
  - Deterministic behavior under test.
  - Strong observability (logging and metrics).
  - Easy unit/integration testing without a real network.

2. Layered architecture
We conceptually split the network system into four layers:

2.1 Message layer (application-facing)
- Defines a common message envelope and delivery semantics.
- Provides serialization/deserialization and reliability helpers.
- Exposes a transport-agnostic API to the rest of the system.

2.2 Transport abstraction layer
- Presents a consistent interface for sending/receiving message envelopes.
- Hides TCP/UDP differences behind a single API.
- Knows about priorities, reliability modes, and channels, but not application semantics.

2.3 Impairment engine
- Sits logically between transport abstraction and underlying I/O.
- Applies configured impairments (latency, jitter, loss, duplication, reordering, partitions).
- Must be usable with both real sockets and the local simulation harness.

2.4 Platform/network backend
- Encapsulates raw socket operations (TCP, UDP) and any TLS/DTLS.
- Handles connection management, framing, basic error mapping, and OS specifics.
- No application policy: only transport mechanics.

3. Message envelope and semantics

3.1 Message envelope
Every logical application message must be carried inside a standard envelope with at least:
- [REQ-3.1] message_type: enumerated type or similar.
- [REQ-3.1] message_id: unique per sender or globally; sufficient for dedupe.
- [REQ-3.1] timestamp: creation or enqueue time.
- [REQ-3.1] source_id, destination_id: node/endpoint identifiers.
- [REQ-3.1] priority: used for channel prioritization.
- [REQ-3.1] reliability_class: e.g., best_effort, reliable_ack, reliable_retry.
- [REQ-3.1] expiry_time: wall-clock or relative; used to drop stale messages.
- [REQ-3.1] payload: opaque, serialized bytes.

3.2 Messaging utilities
The message layer must provide helpers for:
- [REQ-3.2.1] Message ID generation and comparison.
- [REQ-3.2.2] Timestamps and expiry checking.
- [REQ-3.2.3] Serialization/deserialization:
  - Deterministic, endian-safe, version-tolerant where possible.
- [REQ-3.2.4] ACK handling:
  - Generating ACK/NAK messages where needed.
  - Tracking outstanding messages awaiting ACK.
- [REQ-3.2.5] Retry logic:
  - Configurable retry count and backoff.
  - Clear distinction between "in progress", "failed", and "expired".
- [REQ-3.2.6] Duplicate suppression:
  - Recognize and drop duplicates based on (source_id, message_id) pairs.
- [REQ-3.2.7] Expiry handling:
  - Drop or deprioritize expired messages before delivery.

3.3 Delivery semantics
The system must support multiple delivery semantics, including:
- [REQ-3.3.1] Best effort:
  - No retry, no ACK; may be dropped without notification.
- [REQ-3.3.2] Reliable with ACK:
  - Single send, expect ACK; treat missing ACK as failure (no automatic retry).
- [REQ-3.3.3] Reliable with retry and dedupe:
  - Retry until ACK or expiry, deduplicating on the receiver.
- [REQ-3.3.4] Expiring messages:
  - Do not deliver messages after expiry_time has passed.
- [REQ-3.3.5] Ordered vs unordered:
  - Support channels where ordering must be preserved.
  - Support channels where ordering does not matter and may be relaxed for performance.

4. Transport abstraction API

4.1 Core operations
The transport abstraction must offer (conceptually) functions like:
- [REQ-4.1.1] init(config):
  - Initialize transport/impairment state; perform any allowed allocation here.
- [REQ-4.1.2] send_message(envelope):
  - Queue/send a single message envelope via an appropriate channel.
- [REQ-4.1.3] receive_message(timeout):
  - Block or poll until a message envelope is received or timeout occurs.
- [REQ-4.1.4] flush/close:
  - Gracefully flush pending messages (where applicable) and close resources.

4.2 Configuration and channels
- [REQ-4.2.1] Support logical channels (or queues) with:
  - Priority level.
  - Reliability mode (best-effort vs reliable).
  - Ordering requirement.
- [REQ-4.2.2] Allow configuration of:
  - Timeouts (send, receive, connect).
  - Per-channel limits (queue depth, rate limits).
  - Which impairments apply to which channels.

5. Impairment engine requirements

5.1 Impairment types
The impairment engine must be able to apply, per channel or globally:
- [REQ-5.1.1] Fixed latency (e.g., always add 3 seconds).
- [REQ-5.1.2] Random jitter around a configured mean/variance.
- [REQ-5.1.3] Packet loss with configurable probability.
- [REQ-5.1.4] Packet duplication with configurable probability.
- [REQ-5.1.5] Packet reordering:
  - Buffer messages and release them out of order according to policy.
- [REQ-5.1.6] Partitions / intermittent outages:
  - Drop or delay all traffic for specified intervals to simulate link loss.

5.2 Configuration and control
- Impairments must be configurable through:
  - [REQ-5.2.1] A structured configuration object or file (not ad-hoc globals).
- It must be possible to:
  - [REQ-5.2.2] Enable/disable each impairment independently.
  - [REQ-5.2.3] Configure parameters per channel or per peer where sensible.
- Provide:
  - [REQ-5.2.4] A deterministic mode controlled by a seedable PRNG.
  - [REQ-5.2.5] A way to snapshot or log impairment decisions for debugging tests.

5.3 Determinism and testability
- Design the impairment engine so:
  - [REQ-5.3.1] Given the same seed and inputs, the sequence of impairments is reproducible.
  - [REQ-5.3.2] It can run against the local simulation harness without real sockets.
- Keep APIs simple and composable to allow injection/mocking in tests.

6. Platform/network backend requirements

6.1 TCP (connection-oriented) backend
- Connection management:
  - [REQ-6.1.1] Perform standard 3-way handshake to establish connections.
  - [REQ-6.1.2] Implement configurable connection timeouts and retry logic.
  - [REQ-6.1.3] Support graceful disconnect (FIN/ACK) and handle abrupt disconnects (RST, failures).
- Reliability:
  - Use TCP's guaranteed, in-order delivery, but still detect/handle application-level failures.
  - [REQ-6.1.4] Implement application-level flow control as needed (e.g., queue limits).
- Data framing:
  - Implement explicit message framing over the TCP byte stream:
    - [REQ-6.1.5] Typically length-prefixed or header-with-length framing.
  - [REQ-6.1.6] Correctly handle partial reads and writes; do not assume one send == one receive.
- Concurrency:
  - [REQ-6.1.7] Support multiple simultaneous connections.
  - Use a well-defined model (thread-per-connection, thread pool, or event loop) consistent with the global rules.

6.2 UDP (connectionless) backend
- Reliability:
  - [REQ-6.2.1] No inherent reliability; implement optional ACK/NAK + retry where required.
  - Handle packet loss gracefully.
- Ordering:
  - [REQ-6.2.2] Use sequence numbers when ordering is needed.
  - Detect and handle duplicates and out-of-order packets.
- Message size:
  - [REQ-6.2.3] Respect UDP size limits.
  - If needed, implement fragmentation/reassembly at the message layer.
- Spoofing and validation:
  - [REQ-6.2.4] Validate source address and basic sanity of incoming datagrams.

6.3 Common socket requirements (TCP and UDP)
- Addressing:
  - [REQ-6.3.1] Bind to configurable IP/port pairs.
  - Support IPv4, and optionally IPv6.
  - Use appropriate socket options (e.g., SO_REUSEADDR) to avoid common startup issues.
- Error handling:
  - Explicitly handle all error codes (connection refused, reset, timeouts, etc.).
  - [REQ-6.3.2] Classify errors into recoverable vs fatal, mapping to WARNING_LO / WARNING_HI / FATAL as appropriate.
- Timeouts and liveness:
  - [REQ-6.3.3] Implement read/write timeouts to avoid indefinite blocking.
  - Provide heartbeat/keepalive mechanisms for long-lived connections where appropriate.
- Security:
  - [REQ-6.3.4] Provide a clear extension point for TLS (TCP) and DTLS (UDP) usage.
  - Design interfaces so that secure vs insecure transports can be swapped without changing higher layers.
- Buffering and scalability:
  - [REQ-6.3.5] Do not assume unbounded buffers; make buffer sizes explicit and configurable.
  - Choose concurrency models that can scale to expected connection/message volumes.

7. Logging, metrics, and observability

7.1 Logging
- Log at least:
  - [REQ-7.1.1] Connection establishment and teardown.
  - [REQ-7.1.2] Major state changes (e.g., entering/exiting partition, switching transport).
  - [REQ-7.1.3] Errors and FATAL events with enough context to debug.
- [REQ-7.1.4] Avoid logging full payload contents by default.
  - Prefer logging message IDs, types, sizes, and metadata.

7.2 Metrics
- Expose hooks or counters for:
  - [REQ-7.2.1] Latency distributions (end-to-end and per-hop if possible).
  - [REQ-7.2.2] Packet/message loss, duplication, reordering rates.
  - [REQ-7.2.3] Retry counts, timeouts, and failures.
  - [REQ-7.2.4] Connection counts and restart/fatal event counts.

8. Interaction with global standards

8.1 Compliance expectations
- All application-specific code must:
  - Respect the global C/C++ standard in .claude/CLAUDE.md.
  - Fit into the layered architecture defined there and here.
- If a networking requirement appears to conflict with the global standards:
  - Prefer the safer interpretation.
  - Document and minimize any necessary trade-offs.

8.2 Design patterns
- Favor simple, explicit state machines over complex control flow.
- Prefer small, well-defined modules that map cleanly to requirements (message layer, transport, impairment, backend).
- Always keep testability and determinism in mind, especially for impairment and retry logic.

---

13. Software Safety Requirements (NASA-STD-8719.13C / NASA-STD-8739.8A)

```xml
<software_safety_requirements>
This section implements the software safety analysis obligations of NASA-STD-8719.13C
(Software Safety) and NASA-STD-8739.8A (Software Assurance) for this project.

1. Mandatory artifacts
The following artifacts must exist, be kept under version control, and be updated
whenever the software changes in a way that affects safety properties:

  a) Software Safety Hazard Analysis — docs/HAZARD_ANALYSIS.md §1
     A table of software-induced hazards, each with:
       - Unique hazard ID (HAZ-NNN)
       - Description of the hazardous condition
       - Severity category (Cat I–IV per NASA-STD-8719.13C §4.3)
       - Triggering condition (what software state produces the hazard)
       - Detection mechanism (how it is caught)
       - Mitigation (the code-level defense)
     Update trigger: any new message path, new reliability mode, or new transport.

  b) Failure Mode and Effects Analysis (FMEA) — docs/HAZARD_ANALYSIS.md §2
     Per major component: failure mode, system-level effect, severity, detection,
     and mitigation. Components in scope: DeliveryEngine, RetryManager, AckTracker,
     DuplicateFilter, Serializer, TcpBackend, UdpBackend, ImpairmentEngine.
     Update trigger: any new component or modification to an existing component's
     public interface or state machine.

  c) Safety-Critical Function Classification — docs/HAZARD_ANALYSIS.md §3
     Every public function/method in src/ is classified as either:
       - Safety-Critical (SC): on the send/receive/expiry/dedup/retry/impairment path;
         directly mitigates one or more HAZ-NNN entries.
       - Non-Safety-Critical (NSC): observability, configuration, lifecycle init,
         or test-only; no direct effect on message delivery semantics.
     SC functions must also carry a source annotation in their header declaration
     (see rule 2 below).
     Update trigger: any new public function or change to an existing function's role.

2. Source annotation for SC functions
Every function classified SC in docs/HAZARD_ANALYSIS.md §3 must carry the following
comment on its declaration in the corresponding .hpp file:
    // Safety-critical (SC): HAZ-NNN[, HAZ-NNN...]
The HAZ IDs must match entries in docs/HAZARD_ANALYSIS.md §1. Unannotated SC
declarations are a defect; annotated NSC declarations are a documentation error.

3. Review obligations
- SC functions require mandatory peer review before merge (no self-merge).
- SC functions must have ≥ branch coverage in tests; MC/DC coverage is the target.
- Any change to an SC function triggers a re-review of all FMEA entries that
  reference that function.

4. Classification change process
Reclassifying a function from NSC to SC, or adding a new SC function, requires:
  1. A new or updated HAZ-NNN entry in docs/HAZARD_ANALYSIS.md §1.
  2. Updated FMEA rows in §2.
  3. Source annotation added to the .hpp declaration.
  4. Reviewer sign-off before merge.
</software_safety_requirements>
```

---

14. Coverage Requirements (NASA-STD-8739.8A / NPR 7150.2D)

```xml
<coverage_requirements>
1. Mandatory floor — all SC functions (docs/HAZARD_ANALYSIS.md §3):
   - Branch coverage (true/false for every decision) is the CI-enforced minimum.
   - Measure with: make coverage  (LLVM source-based coverage via clang++).
   - A branch coverage regression on any SC function is a blocking defect.

2. MC/DC goal — the five highest-hazard SC functions:
   - DeliveryEngine::send         (HAZ-001, HAZ-002, HAZ-006)
   - DeliveryEngine::receive      (HAZ-001, HAZ-003, HAZ-004, HAZ-005)
   - DuplicateFilter::check_and_record  (HAZ-003)
   - Serializer::serialize        (HAZ-005)
   - Serializer::deserialize      (HAZ-001, HAZ-005)
   MC/DC requires every condition in each decision to independently affect the
   outcome. Demonstrate MC/DC by test-case review, not tooling alone.

3. NSC functions: line coverage is sufficient; no branch-coverage enforcement.

4. Per-file branch coverage thresholds and known ceilings:
   The policy floor is ≥ 75% branch coverage for all SC function files.
   Serializer.cpp has a proven architectural ceiling of 74.36% (58/78 branches).
   This ceiling arises because NEVER_COMPILED_OUT_ASSERT expands to
   `if (!(cond)) { ...; abort(); }`, and LLVM source-based coverage counts
   the True (fires) branch as a branch but does not insert a profile counter
   for it (the path is [[noreturn]] via abort()). The result is 20 permanently-
   missed branches — one per NEVER_COMPILED_OUT_ASSERT call across the 8
   functions in Serializer.cpp (6 helpers × 2 asserts + serialize × 4 +
   deserialize × 4). All 18 decision-level branches (the actual serialization
   and deserialization logic) are 100% covered. The Serializer.cpp threshold
   is therefore set at 74% (maximum achievable); this is not a defect and
   requires no remediation.
   ImpairmentEngine.cpp has a proven architectural ceiling of 74.19% (138/186
   branches). The ceiling arises from two sources: (a) 40 permanently-missed
   branches from NEVER_COMPILED_OUT_ASSERT calls (one per assertion, [[noreturn]]
   True path); and (b) 8 architecturally-impossible logic branches:
     - queue_to_delay_buf L90:27 False: loop always exits early via return (the
       assert at L87 guarantees a free slot exists, so the bound is never hit).
     - process_outbound L198:9 True: queue_to_delay_buf always succeeds when
       called because L192 already checked m_delay_count < IMPAIR_DELAY_BUF_SIZE.
     - apply_duplication L139:17 False: queue_to_delay_buf always succeeds when
       called from L138 (L137:13 True guarantees a free slot).
     - process_inbound L292:9 False: the entry assert requires buf_cap > 0U.
     - process_inbound L305:13 False: after decrement, m_reorder_count is always
       < m_cfg.reorder_window_size (mathematically guaranteed post-decrement).
     - Three additional branches within NEVER_COMPILED_OUT_ASSERT macro expansion.
   All reachable decision-level branches are 100% covered. The ImpairmentEngine.cpp
   threshold is therefore set at 74% (maximum achievable); this is not a defect
   and requires no remediation.
   TlsTcpBackend.cpp meets the ≥ 76% target at 76.24% (231/303 branches).
   AckTracker, RetryManager, and DeliveryEngine unit tests have been added
   (tests/test_AckTracker.cpp, tests/test_RetryManager.cpp,
   tests/test_DeliveryEngine.cpp) and all meet the ≥ 75% branch coverage floor.

5. Coverage does not substitute for code review or static analysis. All three
   (coverage, review, static analysis) are required for SC functions.
</coverage_requirements>
```

---

15. Stack Depth Analysis (NASA-STD-8719.13C)

```xml
<stack_depth_requirements>
1. Basis: Power of 10 Rule 1 (no recursion) makes the call graph a DAG.
   Worst-case stack depth is the longest path in the DAG and can be determined
   by static inspection without instrumentation tools.

2. Analysis artifact: docs/STACK_ANALYSIS.md
   Contains the four worst-case call chains, per-frame size estimates, and
   platform headroom calculations.

3. Current worst case: 9 frames, ~748 bytes peak (Chain 1 — outbound send).
   Platform headroom: > 700× on macOS/Linux. No stack overflow risk.

4. Update trigger: this document must be updated when:
   - Any function introduces a stack-allocated buffer larger than 256 bytes.
   - A new call chain extends depth beyond 9 frames.
   - A new thread entry point is added.

5. Embedded porting: on targets with ≤ 64 KB stack, re-derive estimates using
   a stack-analysis tool (avstack, StackAnalyzer, or linker map inspection).
   The no-recursion guarantee remains the primary protection.
</stack_depth_requirements>
```

---

16. Worst-Case Execution Time Analysis (NASA-STD-8719.13C)

```xml
<wcet_requirements>
1. Basis: Power of 10 Rule 2 (all loops have statically provable bounds) means
   every SC function's execution time is expressible as a closed-form formula
   in the compile-time capacity constants from src/core/Types.hpp.

2. Analysis artifact: docs/WCET_ANALYSIS.md
   Contains per-SC-function worst-case operation counts and guidance for
   converting to actual cycle counts on deterministic embedded targets.

3. Classical WCET measurement (aiT, Rapita, etc.) is not applicable on a
   POSIX host with non-deterministic scheduling. On a deterministic embedded
   target, use the operation counts in docs/WCET_ANALYSIS.md × target
   cycles-per-operation to derive a true WCET bound.

4. Dominant cost paths (see docs/WCET_ANALYSIS.md for full table):
   - DeliveryEngine::pump_retries()  O(ACK_TRACKER_CAPACITY × MSG_MAX_PAYLOAD_BYTES)
   - Serializer::serialize/deserialize  O(MSG_MAX_PAYLOAD_BYTES)
   - DuplicateFilter::check_and_record  O(DEDUP_WINDOW_SIZE)

5. Update trigger: this document must be updated when any capacity constant
   in src/core/Types.hpp changes, or when a new SC function is added.
</wcet_requirements>
```

---

17. Formal Methods and Software Classification (NASA-STD-8719.13C / NPR 7150.2D)

```xml
<formal_methods_requirements>
1. Software classification: Class C (NPR 7150.2D Appendix D)
   Rationale: messageEngine is infrastructure software — a networking library.
   It does not directly command actuators or safety barriers. The application
   embedding it is responsible for Class A/B assurance if required.

2. Current formal specification: docs/STATE_MACHINES.md
   Explicit state-transition tables for the three safety-critical state
   machines: AckTracker (§1), RetryManager (§2), ImpairmentEngine (§3).
   This lightweight specification satisfies Class C review requirements.

3. Reclassification trigger — if this library is reclassified to Class A or B
   (e.g., embedded directly in a flight computer as the sole comm layer without
   an overlying application safety barrier), the following obligations apply:
   - Model checking of AckTracker, RetryManager, ImpairmentEngine state machines
     using TLA+ or SPIN, verifying all invariants in docs/STATE_MACHINES.md.
   - Theorem proving of Serializer bounds using Frama-C (WP plugin) or Coq.
   - Proof of retry termination (every WAITING slot eventually reaches INACTIVE).
   - Independent V&V of all SC functions (NPR 7150.2D §3.11).

4. State machine update trigger: docs/STATE_MACHINES.md must be updated when
   any state, transition, guard, or invariant changes in AckTracker,
   RetryManager, or ImpairmentEngine source code.
</formal_methods_requirements>
```

---

9. Power of 10 / MISRA compliance table by directory

This table resolves the conflict between .claude/CLAUDE.md §1 ("all code must obey Power of 10")
and .claude/CLAUDE.md §3 Architecture ("tests may relax some rules where necessary"). The
contradiction was identified during a standards review (Conflict #3). This table is the authoritative
resolution: it defines exactly which rules apply to production code and which are explicitly exempted
for test code only. All exemptions must be justified in a comment at the point of use.

Rule                                   | src/core | src/platform | src/app | tests/
---------------------------------------|----------|--------------|---------|---------
Rule 1: no goto, no recursion          | FULL     | FULL         | FULL    | FULL
Rule 2: fixed loop bounds              | FULL *   | FULL *       | FULL *  | FULL *
Rule 3: no dynamic allocation          | FULL     | FULL         | FULL    | EXEMPT **
Rule 4: short functions (CC <= 10)     | FULL     | FULL         | FULL    | FULL
Rule 5: assertion density              | FULL     | FULL         | FULL    | EXEMPT ***
Rule 6: minimal variable scope         | FULL     | FULL         | FULL    | FULL
Rule 7: check all return values        | FULL     | FULL         | FULL    | FULL
Rule 8: limit preprocessor             | FULL     | FULL         | FULL    | FULL
Rule 9: no explicit function pointers  | FULL     | FULL         | FULL    | EXEMPT ****
Rule 10: zero warnings                 | FULL     | FULL         | FULL    | FULL
No STL (except std::atomic)            | FULL     | FULL         | FULL    | EXEMPT *****
MISRA C++:2023                         | FULL     | FULL         | FULL    | ADVISORY

*     Rule 2 infrastructure deviation applies — see .claude/CLAUDE.md §2.2 exception.
**    Test frameworks (e.g., Catch2, Google Test) require dynamic allocation internally;
      exemption is limited to test harness setup and teardown, not logic under test.
***   Test framework assertions (ASSERT_EQ, REQUIRE, etc.) satisfy the intent of Rule 5;
      native assert() density requirement is waived in tests/.
****  Test frameworks use callbacks and function pointers internally; exemption is limited
      to framework integration points, not test logic itself.
***** STL containers and algorithms are permitted in tests/ for test data setup only.

10. NEVER_COMPILED_OUT_ASSERT — assertion policy (resolves Conflict #4)

This section resolves the conflict between .claude/CLAUDE.md §8 ("fail fast in debug builds
while preserving safe behavior in production") and JPL Power of 10 Rule 5, which requires
assertions to remain active in all builds including production. Disabling assertions via
NDEBUG in the safety-critical build defeats Rule 5 entirely.

Resolution: raw assert() is prohibited in production code. All production assertions must
use the project macro NEVER_COMPILED_OUT_ASSERT(cond), which is never compiled out.

10.1 Macro behavior by build type

  Build type   | Behavior when cond is false
  -------------|---------------------------------------------------------------
  Debug / test | Logs condition + file + line, then calls abort().
               | Produces immediate crash and a full stack trace for debugging.
  Production   | Logs FATAL with condition + file + line, then triggers a
               | controlled component reset. Does not call abort() or terminate
               | the process. Allows the system to recover at the component level.

10.2 Usage rules

- Use NEVER_COMPILED_OUT_ASSERT(cond) everywhere assert() would have been used in
  production code (src/core, src/platform, src/app).
- Raw assert() is permitted in tests/ only, where abort()-on-failure is acceptable.
- The macro must log at minimum: condition text, file name, line number, severity FATAL.
- The production reset path must be defined before the macro is used in any module;
  a default no-op reset is not acceptable for safety-critical components.

10.3 TODO: implement NEVER_COMPILED_OUT_ASSERT

- [ ] Define NEVER_COMPILED_OUT_ASSERT in src/core/Logger.hpp or a dedicated
      src/core/Assert.hpp header.
- [ ] Define and register the production component-reset handler.
- [ ] Replace all existing raw assert() calls in src/ with NEVER_COMPILED_OUT_ASSERT.
- [ ] Add Clang-Tidy or cppcheck rule to flag any remaining assert() in src/.

---

9.1 Named static analysis toolchain (NPR 7150.2D / NASA-STD-8739.8)

NASA projects must identify static analysis tools explicitly because different tools have
different rule sets, MISRA coverage levels, and false-positive rates (NPR 7150.2D §3).
"Static analysis runs regularly" is not sufficient — the specific tools, their roles, and
their configurations must be named. This section is the authoritative toolchain definition
for messageEngine.

Tools are organised into three tiers by when they run and what they enforce.

────────────────────────────────────────────────────────────────────────────────
Tier 1 — Build-time (every compile, already active)
────────────────────────────────────────────────────────────────────────────────

Tool: GCC / Clang compiler warnings
  Flags: -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
         -Wcast-align -Wformat=2 -Wnull-dereference -Wdouble-promotion
  Role:  First line of defence. Zero-warnings policy (Power of 10 Rule 10) means
         any warning is a build failure. Already enforced in Makefile.
  Status: ACTIVE

────────────────────────────────────────────────────────────────────────────────
Tier 2 — Fast CI pass (run on every change, before merge)
────────────────────────────────────────────────────────────────────────────────

Tool: Clang-Tidy  (free — https://clang.llvm.org/extra/clang-tidy/)
  Makefile target: make lint
  Role:  Day-to-day enforcement of Power of 10 patterns, MISRA-adjacent checks,
         complexity limits, and the per-directory compliance table (§9).
  Config: src/.clang-tidy  — strict profile (full rule set)
          tests/.clang-tidy — relaxed profile (STL and dynamic alloc permitted)
  Key checks to enable:
    - readability-function-cognitive-complexity (threshold: 10, per Rule 4/§4)
    - cppcoreguidelines-no-malloc (Rule 3)
    - fuchsia-restrict-system-includes (no STL in src/)
    - misc-no-recursion (Rule 1)
    - bugprone-*, performance-*, portability-*
  Status: TODO — see §9.2

Tool: Cppcheck  (free — https://cppcheck.sourceforge.io/)
  Makefile target: make cppcheck
  Role:  Second-pass analysis; catches patterns Clang-Tidy misses — integer overflow,
         uninitialised variables, out-of-bounds array access, and MISRA C++:2023
         rules via the bundled misra.py addon.
  Config: Run with --addon=misra --misra-c++-version=2023 on src/ only.
          Use --suppress-file=.cppcheck-suppress for documented deviations.
  Status: TODO — see §9.2

────────────────────────────────────────────────────────────────────────────────
Tier 3 — MISRA compliance pass (run before any release or external review)
────────────────────────────────────────────────────────────────────────────────

Primary MISRA checker:
Tool: PC-lint Plus  (commercial — https://gimpel.com)
  Makefile target: make pclint
  Role:  Authoritative MISRA C++:2023 compliance checker. Produces the compliance
         report required for NPR 7150.2D formal assurance reviews. PC-lint Plus is
         the most widely accepted MISRA C++ checker in NASA and DO-178C contexts.
         Covers all Required and Advisory rules; generates per-rule deviation reports.
  Config: pclint/co-gcc.lnt + pclint/misra_cpp_2023.lnt (to be created).
         Separate .lnt option files for src/ (strict) and tests/ (relaxed).
  Status: TODO — requires licence purchase; see §9.2

Alternative MISRA checker (if PC-lint Plus is not available):
Tool: Cppcheck with MISRA C++:2023 addon  (free, partial coverage)
  Role:  Covers a subset of MISRA C++:2023 rules. Acceptable for development-time
         checking but does not produce a full compliance report suitable for formal
         audit. Use as a stand-in until PC-lint Plus is procured.
  Status: TODO — same Cppcheck install as Tier 2; add --addon=misra flag.

────────────────────────────────────────────────────────────────────────────────
Optional Tier 4 — Deep formal analysis (pre-release or on CRITICAL changes)
────────────────────────────────────────────────────────────────────────────────

Tool: Polyspace Bug Finder / Code Prover  (commercial — MathWorks)
  Role:  Formal proof of absence of runtime errors (null deref, overflow, data races).
         Required for Class A software under NPR 7150.2D. Optional for this project
         unless safety classification is raised. Produces green/red/orange proof results
         per code region.
  Status: NOT REQUIRED at current classification; revisit if classification changes.

Tool: Coverity Static Analysis  (commercial — Synopsys)
  Role:  Deep interprocedural analysis; excellent at finding concurrency defects and
         subtle memory errors. Widely used by NASA and DoD projects.
  Status: NOT REQUIRED at current classification; revisit if classification changes.

────────────────────────────────────────────────────────────────────────────────

9.2 TODO: configure and integrate Tier 2 and Tier 3 tools

- [x] Install Clang-Tidy; create src/.clang-tidy (strict) and tests/.clang-tidy (relaxed).
- [x] Add `make lint` target invoking Clang-Tidy on src/ and tests/.
- [x] Install Cppcheck; create .cppcheck-suppress deviation file.
- [x] Add `make cppcheck` target invoking Cppcheck + MISRA addon on src/.
- [ ] Procure PC-lint Plus licence; create pclint/ config directory.
- [ ] Add `make pclint` target for formal MISRA C++:2023 compliance report.
- [ ] Add `make static_analysis` umbrella target running lint + cppcheck in sequence.
- [ ] Document all tool-reported deviations in docs/DEFECT_LOG.md with WAIVE disposition
      and the specific MISRA rule reference.

11. Traceability policy (NPR 7150.2D — bidirectional traceability)

This section establishes the traceability rule required to comply with NPR 7150.2D §3.
REQ IDs are defined inline in §§3–7 above using the format [REQ-x.x].

11.1 Rules for all contributors and AI assistants
- Every source file in src/ must contain at least one comment of the form:
    // Implements: REQ-x.x[, REQ-y.y ...]
  placed in the file-level header block.
- Every test file in tests/ must contain at least one comment of the form:
    // Verifies: REQ-x.x[, REQ-y.y ...]
  placed in the file-level header block AND on each individual test function
  that exercises a specific requirement.
- Do not introduce new functionality without first assigning it a REQ ID here.
- Run docs/check_traceability.sh after any change to verify no file is missing
  a tag and no REQ ID is orphaned (defined but not implemented or not verified).

11.2 Traceability matrix
The live bidirectional matrix is maintained in docs/TRACEABILITY_MATRIX.md.
It is generated from the Implements/Verifies comments by docs/check_traceability.sh.
Do not edit the matrix by hand; regenerate it by running the script.

---

12. Formal inspection and peer review process (NPR 7150.2D §3 / NASA-STD-8739.8)

This section replaces the prior placeholder ("assume formal inspections") with a defined,
lightweight structured review process satisfying NPR 7150.2D intent without full Fagan
overhead. Every non-trivial change to src/ or tests/ requires a completed inspection record.

12.1 Reviewer roles

  Author     — wrote the code; presents the change; may not moderate their own review.
  Moderator  — owns the checklist (docs/INSPECTION_CHECKLIST.md); facilitates; signs off;
               records all defects in docs/DEFECT_LOG.md.
  Reviewer   — at least one independent reviewer required. For AI-assisted development
               the human engineer fills this role, reviewing all AI-generated output as
               if it were written by a junior engineer.

12.2 Entry criteria (all must be true before review begins)

  [ ] make passes with zero warnings and zero errors.
  [ ] make check_traceability passes with RESULT: PASS.
  [ ] make run_tests passes — all tests green.
  [ ] All new or modified src/ files carry // Implements: REQ-x.x tags.
  [ ] All new or modified tests/ files carry // Verifies: REQ-x.x tags.
  [ ] No raw assert() in src/ — NEVER_COMPILED_OUT_ASSERT used throughout.
  [ ] No dynamic allocation on critical paths after init (Power of 10 Rule 3).
  [ ] Author has self-reviewed against docs/INSPECTION_CHECKLIST.md before
      requesting a moderator-led review.

12.3 Review execution

  - Author walks through each changed file, explaining intent and design decisions.
  - Moderator works through docs/INSPECTION_CHECKLIST.md line by line.
  - Every defect found is logged immediately in docs/DEFECT_LOG.md with:
      date, file:line, defect description, severity (MINOR / MAJOR / CRITICAL),
      disposition (FIX / WAIVE / DEFER), and resolution notes.
  - Severity definitions:
      CRITICAL — safety, correctness, or security issue; must be fixed before exit.
      MAJOR    — standards violation or logic error; must be fixed or waived with
                 written justification before exit.
      MINOR    — style, clarity, or advisory; may be deferred with a tracked note.

12.4 Exit criteria (all must be true before change is accepted)

  [ ] All CRITICAL and MAJOR defects are dispositioned (fixed or waived in DEFECT_LOG.md).
  [ ] Checklist is fully completed — no items left blank.
  [ ] make, make run_tests, and make check_traceability all still pass after fixes.
  [ ] Moderator signs off in DEFECT_LOG.md with name/alias and date.

12.5 Waiver policy

  A defect may be waived (not fixed) only if:
  - The waiver is logged in DEFECT_LOG.md with explicit written justification.
  - The waiver references the specific rule being deviated from.
  - The moderator and at least one reviewer agree.
  CRITICAL defects may not be waived; they must be fixed.

12.6 Records retention

  docs/DEFECT_LOG.md is a permanent project record. Entries must never be deleted.
  Entries may be updated to record resolution but the original finding must remain visible.

</application_requirements>
