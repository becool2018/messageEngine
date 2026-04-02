<application_requirements>
NOTE: Global portable C/C++ coding standards (Power of 10, MISRA, F´ subset) are defined in .claude/CLAUDE.md.
Project-specific architecture, layering rules, verification policy, and CI/commit rules are defined in §§1a–1d of this file.
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

1a. Verification Policy (VVP-001)

Before writing or reviewing any test, read `.claude/VERIFICATION_POLICY.md` (VVP-001). It defines the minimum
verification methods (M1–M7) required at each software classification level (Class C / B / A) and the rules
governing architectural ceiling arguments, fault injection, and injectable interface requirements.
The table in §4.1 is normative: a method not listed as sufficient for a given classification is explicitly
insufficient regardless of its quality. Key rules to apply immediately:
  - "Code review alone" (M1 only) never satisfies Class C for SC functions.
  - A loopback-only test environment satisfies M4 only for branches reachable under loopback.
    Dependency-failure branches require M5.
  - Architectural ceilings are a Class C instrument only; they do not apply at Class B or A
    for dependency-failure paths (VVP-001 §4.3 e-i).

1b. Architecture, Layering, and Directory Rules for AI

The following directory-specific rules govern how code is organized in this project and must be
followed by all contributors and AI assistants when editing or generating code:

- src/core:
  - Core domain logic.
  - Depends only on abstract interfaces declared in src/platform (e.g., TransportInterface);
    no direct POSIX, socket, or thread API calls.
  - Must not include or call concrete platform implementations directly.
- src/platform:
  - OS and platform adapters (sockets, timers, threading, filesystem).
  - No application-specific policy; only mechanics and thin wrappers.
- src/app:
  - Application orchestration and high-level behavior.
  - Uses src/core and src/platform through their public interfaces.
- tests/:
  - Unit and integration tests.
  - Must obey all non-negotiable rules: no undefined behaviour, no C-style casts,
    no goto/recursion, no exceptions (compile flag enforced), no C++20+ features,
    zero compiler warnings, and all return values checked.
  - May relax the following rules to accommodate test-framework idioms:
    - STL containers (e.g., std::vector, std::string) are permitted for test
      fixture setup and data construction only; not on paths under test.
    - Templates are permitted for test helper utilities (e.g., typed test tables).
    - Dynamic allocation (new/delete) is permitted in test fixture setup and
      teardown; not on the path that exercises production code.
    - The per-function assertion minimum (Rule 5) is relaxed to one assertion per
      test function when the function body is a single VERIFY/CHECK call.
    - Cyclomatic complexity ceiling (Rule 4, ≤10) is raised to ≤15 for test
      functions that necessarily exercise many code paths in sequence.
    - MISRA advisory rules may be relaxed where the test framework requires it;
      MISRA required rules remain in force.
  - No other relaxations are permitted. When in doubt, follow the production rules.
- docs/:
  - Requirements, design docs, and other project documentation.

Layering rules:
- Higher layers may depend on lower ones (app → core → platform), never the reverse.
- No bypassing abstractions:
  - src/app and src/core must not call raw OS APIs or sockets directly; only through
    src/platform interfaces.
- No cyclic dependencies between modules or directories.

Before making significant changes, briefly summarize how the planned change fits:
  - The global C/C++ coding standard (.claude/CLAUDE.md).
  - The layering rules above.
  - Any relevant module requirements in §§3–7 of this file.

1c. CI/Commit Rules

Before every git commit, run `make lint` and `make run_tests`. Both must pass with zero errors
before the commit is created. If either fails, fix the issue first — do not commit broken or
lint-failing code. These checks are also entry criteria for the formal inspection process (§12).

1d. Resolved Design Decisions

The following questions were open during initial design and are now resolved:
- MISRA versions: MISRA C:2025 for C code, MISRA C++:2023 for C++ code.
- Compiler standard: -std=c++17 in use.
- std::atomic carve-out: std::atomic<T> for integral types is permitted;
  GCC __atomic built-ins are no longer used anywhere in the codebase.
- Performance targets: addressed in §16 (WCET) and docs/WCET_ANALYSIS.md;
  per-module resource limits are the compile-time constants in src/core/Types.hpp.
- Test-only relaxations: see §1b above (Architecture, Layering, and Directory Rules for AI,
  tests/ entry) for the exact enumerated list of permitted relaxations and non-negotiable rules.

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

6.4 DTLS (Datagram TLS over UDP) backend
- DTLS mode:
  - [REQ-6.4.1] Use DTLS transport mode (MBEDTLS_SSL_TRANSPORT_DATAGRAM) for record-layer
    security over UDP; support both DTLS client and server roles.
- Anti-replay and cookie exchange:
  - [REQ-6.4.2] Arm DTLS cookie exchange on the server to prevent amplification attacks;
    bind each cookie to the client's (address, port) transport identity.
- Retransmission timer:
  - [REQ-6.4.3] Configure the DTLS retransmission timer (min/max) to match the expected
    link RTT; use mbedTLS timer callbacks to drive retransmission without busy-waiting.
- MTU enforcement:
  - [REQ-6.4.4] Enforce a configurable DTLS record MTU (DTLS_MAX_DATAGRAM_BYTES) to prevent
    IP fragmentation; reject outbound messages whose serialized size exceeds the MTU.
- Plaintext fallback:
  - [REQ-6.4.5] When `tls_enabled == false`, fall through to the plain UDP socket path
    (no handshake, no certificate loading); allow the same DtlsUdpBackend code to operate
    in plaintext mode for testing without changing higher layers.

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

9.1 Named static analysis toolchain
See docs/STATIC_ANALYSIS_TOOLCHAIN.md for the full toolchain definition (tiers, tool
configs, and TODO status). Summary: Tier 1 (compiler -Wall/-Werror) ACTIVE; Tier 2
(Clang-Tidy `make lint`, Cppcheck `make cppcheck`) ACTIVE; Tier 3 (PC-lint Plus
`make pclint`) PENDING licence purchase.

---

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
  Production   | Logs FATAL with condition + file + line, then calls the
               | registered IResetHandler::on_fatal_assert() via virtual dispatch
               | (Power of 10 Rule 9 vtable exception). Falls back to ::abort()
               | if no handler is registered. Sets g_fatal_fired if the handler
               | returns (embedded soft-reset path).

10.2 Usage rules

- Use NEVER_COMPILED_OUT_ASSERT(cond) everywhere assert() would have been used in
  production code (src/core, src/platform, src/app).
- Raw assert() is permitted in tests/ only, where abort()-on-failure is acceptable.
- The macro must log at minimum: condition text, file name, line number, severity FATAL.
- Register a handler via assert_state::set_reset_handler() before using any component;
  a default no-op reset is not acceptable for safety-critical components.
- For POSIX builds register AbortResetHandler::instance() (src/core/AbortResetHandler.hpp).
- See src/core/Assert.hpp, src/core/AssertState.hpp, src/core/IResetHandler.hpp.

---

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

Every non-trivial change to src/ or tests/ requires a completed inspection record in
docs/DEFECT_LOG.md. Full procedure and moderator checklist: docs/INSPECTION_CHECKLIST.md.

12.1 Reviewer roles

  Author     — wrote the code; presents the change; may not moderate their own review.
  Moderator  — owns the checklist (docs/INSPECTION_CHECKLIST.md); facilitates; signs off;
               records all defects in docs/DEFECT_LOG.md.
  Reviewer   — at least one independent reviewer required. For AI-assisted development
               the human engineer fills this role, reviewing all AI-generated output as
               if it were written by a junior engineer.

12.2 Entry criteria (all must be true before review begins)

  [ ] make passes with zero warnings and zero errors.
  [ ] make lint passes with zero clang-tidy violations (CC ≤ 10 enforced).
  [ ] make check_traceability passes with RESULT: PASS.
  [ ] make run_tests passes — all tests green.
  [ ] All new or modified src/ files carry // Implements: REQ-x.x tags.
  [ ] All new or modified tests/ files carry // Verifies: REQ-x.x tags.
  [ ] No raw assert() in src/ — NEVER_COMPILED_OUT_ASSERT used throughout.
  [ ] No dynamic allocation on critical paths after init (Power of 10 Rule 3).
  [ ] Author has self-reviewed against docs/INSPECTION_CHECKLIST.md before
      requesting a moderator-led review.

12.3 Exit criteria summary
All CRITICAL and MAJOR defects dispositioned; checklist complete; make + run_tests +
check_traceability all pass; moderator signed off in docs/DEFECT_LOG.md.
Full exit criteria, waiver policy, and severity definitions: docs/INSPECTION_CHECKLIST.md.

---

13. Software Safety Requirements (NASA-STD-8719.13C / NASA-STD-8739.8A)

```xml
<software_safety_requirements>
Mandatory safety artifacts (keep under version control; update on any safety-relevant change):
  a) Software Safety Hazard Analysis  — docs/HAZARD_ANALYSIS.md §1
  b) Failure Mode and Effects Analysis (FMEA)  — docs/HAZARD_ANALYSIS.md §2
  c) Safety-Critical Function Classification  — docs/HAZARD_ANALYSIS.md §3

Update triggers:
  - Artifact (a): any new message path, reliability mode, or transport.
  - Artifact (b): any new component or change to a component's public interface or state machine.
  - Artifact (c): any new public function or change to an existing function's role.

1. Source annotation for SC functions
Every function classified SC in docs/HAZARD_ANALYSIS.md §3 must carry the following
comment on its declaration in the corresponding .hpp file:
    // Safety-critical (SC): HAZ-NNN[, HAZ-NNN...]
The HAZ IDs must match entries in docs/HAZARD_ANALYSIS.md §1. Unannotated SC
declarations are a defect; annotated NSC declarations are a documentation error.

2. Review obligations
- SC functions require mandatory peer review before merge (no self-merge).
- SC functions must have 100% branch coverage of all reachable branches in tests;
  MC/DC coverage is the target for the five highest-hazard functions (§14).
- Any change to an SC function triggers a re-review of all FMEA entries that
  reference that function.

3. Classification change process
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
   The policy floor is 100% branch coverage of all reachable branches for every SC
   function file. Per-file architectural ceilings (where the maximum achievable is
   below 100% due to NEVER_COMPILED_OUT_ASSERT [[noreturn]] True paths or proven
   dead code) are documented in docs/COVERAGE_CEILINGS.md.
   A numeric threshold below 100% is only valid when accompanied by a documented
   architectural ceiling in docs/COVERAGE_CEILINGS.md that accounts for every
   single missed branch. Every branch that CAN be exercised by a test MUST be
   exercised.

5. Coverage does not substitute for code review or static analysis. All three
   (coverage, review, static analysis) are required for SC functions.
</coverage_requirements>
```

---

15. Stack Depth Analysis (NASA-STD-8719.13C)

Analysis artifact and current worst-case call chains: docs/STACK_ANALYSIS.md.
Current worst case: 9 frames, ~748 bytes (Chain 1 — outbound send). Platform
headroom: >700× on macOS/Linux.

Update trigger: update docs/STACK_ANALYSIS.md when any function introduces a
stack-allocated buffer >256 bytes, a new call chain exceeds 9 frames, or a new
thread entry point is added.

---

16. Worst-Case Execution Time Analysis (NASA-STD-8719.13C)

Analysis artifact and per-SC-function operation counts: docs/WCET_ANALYSIS.md.
Dominant paths: DeliveryEngine::pump_retries() O(ACK_TRACKER_CAPACITY ×
MSG_MAX_PAYLOAD_BYTES), Serializer::serialize/deserialize O(MSG_MAX_PAYLOAD_BYTES),
DuplicateFilter::check_and_record O(DEDUP_WINDOW_SIZE).

Update trigger: update docs/WCET_ANALYSIS.md when any capacity constant in
src/core/Types.hpp changes or a new SC function is added.

---

17. Formal Methods and Software Classification (NASA-STD-8719.13C / NPR 7150.2D)

```xml
<formal_methods_requirements>
1. Software classification: Class C (NPR 7150.2D Appendix D)
   Rationale: messageEngine is infrastructure software — a networking library.
   It does not directly command actuators or safety barriers. The application
   embedding it is responsible for Class A/B assurance if required.

2. Current verification baseline (per .claude/VERIFICATION_POLICY.md VVP-001 §4.1):
   - SC functions:  M1 (inspection) + M2 (static analysis) + M4 (branch coverage)
   - NSC functions: M1 (inspection) + M2 (static analysis) + M3 (line coverage)
   Although the software classification is Class C, all verification activity
   voluntarily meets Class B requirements (M1 + M2 + M4 + M5 for all functions).

3. Current formal specification: docs/STATE_MACHINES.md
   Explicit state-transition tables for: AckTracker (§1), RetryManager (§2),
   ImpairmentEngine (§3). Update when any state, transition, guard, or invariant
   changes in those components.

4. Reclassification trigger — if reclassified to Class A or B, the following
   obligations apply: TLA+/SPIN model checking of the three state machines;
   Frama-C WP theorem proving of Serializer bounds; proof of retry termination;
   independent V&V of all SC functions (NPR 7150.2D §3.11).
   See TODO_FOR_CLASS_B_CERT.txt for the full gap list.
</formal_methods_requirements>
```

</application_requirements>
