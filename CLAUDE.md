<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Don Jessup. Licensed under the Apache License, Version 2.0. See LICENSE. -->
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

Before writing or reviewing any test, read `docs/VERIFICATION_POLICY.md` (VVP-001). It defines the minimum
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

Before creating or updating a pull request:
1. Run `make pr-audit` (or `bash docs/pr_audit.sh`).
   The script inspects the diff against main, determines which documentation items are
   triggered, verifies each triggered item, and prints a filled-in "Documentation updated"
   block ready to paste directly into the PR description.
2. For any item the audit marks FAIL: complete the required documentation update, then
   re-run `make pr-audit` until all REQUIRED items show PASS or N/A.
3. Paste the printed block into the PR description.  Items the audit cannot verify
   automatically (stress tests, coverage run) are printed as unchecked — confirm them
   manually and tick the box before opening the PR.
The CI workflow pr-checklist.yml blocks merge if any item remains unchecked without N/A.
CI already enforces lint and tests — the PR template covers only the
documentation items that require human judgment.

Branch coverage report (`make coverage`) is required **before any merge to main** for SC
function verification; it is a pre-merge review gate, not a CI automation gate. CI
automatically verifies M2 (lint/static analysis) and M4 dynamic test paths (via
`run_tests` + `run_sanitize`). The human reviewer must confirm no regression in SC function
branch coverage before approving a merge. See docs/COVERAGE_CEILINGS.md for per-file thresholds
and docs/VERIFICATION_POLICY.md §4 for ceiling argument policy.

Stress tests (`make run_stress_tests`) are NOT required on every commit. Run them when any of
the following change:
  - A capacity constant in src/core/Types.hpp (ACK_TRACKER_CAPACITY, DEDUP_WINDOW_SIZE,
    MSG_RING_CAPACITY, IMPAIR_DELAY_BUF_SIZE, MAX_RETRY_COUNT, or similar).
  - A loop bound or slot-management algorithm in AckTracker, RetryManager, RingBuffer,
    or DuplicateFilter.
  - A new platform port or cross-compilation target is introduced.

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
Four layers: Message (app-facing) → Transport abstraction → Impairment engine → Platform/network backend.
Full layer descriptions, directory rules, and architecture diagram: docs/ARCHITECTURE.md.

3. Message envelope and semantics
Full prose specification and rationale: docs/PROTOCOL.md.

3.1 Message envelope
Every logical application message must be carried inside a standard envelope with at least:
- [REQ-3.1] message_type, message_id, timestamp, source_id, destination_id, priority, reliability_class, expiry_time, payload.

3.2 Messaging utilities
- [REQ-3.2.1] Message ID generation and comparison.
- [REQ-3.2.2] Timestamps and expiry checking.
- [REQ-3.2.3] Serialization/deserialization — deterministic, endian-safe; wire format carries version byte (byte 3) and frame magic (bytes 40–41); see REQ-3.2.8.
- [REQ-3.2.4] ACK handling — generate ACK/NAK; track outstanding messages awaiting ACK.
- [REQ-3.2.5] Retry logic — configurable count and backoff; PENDING / FAILED / EXPIRED states.
- [REQ-3.2.6] Duplicate suppression — recognize and drop on (source_id, message_id) pair.
- [REQ-3.2.7] Expiry handling — drop or deprioritize expired messages before delivery.
- [REQ-3.2.8] Wire format version enforcement — version byte must equal PROTO_VERSION; magic word must equal PROTO_MAGIC (both in src/core/ProtocolVersion.hpp). Reject mismatches. Bump PROTO_VERSION on any wire field layout change; PROTO_MAGIC is fixed.
- [REQ-3.2.9] Stale reassembly slot reclamation — sweep slots open longer than recv_timeout_ms to prevent resource exhaustion.
- [REQ-3.2.10] Wire-supplied length overflow guard — validate frame_len and total_payload_length against WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES before arithmetic; return ERR_INVALID + WARNING_HI on violation (HAZ-019, CERT INT30-C).
- [REQ-3.2.11] Constant-time equality — source_id/message_id comparisons in AckTracker, DuplicateFilter, OrderingBuffer must not short-circuit; DTLS cookie compare must be constant-time (HAZ-018, CWE-208).

3.3 Delivery semantics
- [REQ-3.3.1] Best effort — no retry, no ACK; may be dropped silently.
- [REQ-3.3.2] Reliable with ACK — single send; missing ACK = failure; no auto-retry.
- [REQ-3.3.3] Reliable with retry and dedupe — retry until ACK or expiry; deduplicate on receiver.
- [REQ-3.3.4] Expiring messages — do not deliver after expiry_time.
- [REQ-3.3.5] Ordered vs unordered — support both; ordering may be relaxed on unordered channels.
- [REQ-3.3.6] Peer reconnect ordering reset — on HELLO from known peer: reset per-peer sequence state, free held out-of-order messages, discard m_held_pending if it belongs to reconnecting peer. Implemented by: OrderingBuffer::reset_peer(), DeliveryEngine::reset_peer_ordering(), DeliveryEngine::drain_hello_reconnects(), TransportInterface::pop_hello_peer().

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
Full prose specification and rationale: docs/IMPAIRMENT.md.

5.1 Impairment types
The impairment engine must apply, per channel or globally:
- [REQ-5.1.1] Fixed latency.
- [REQ-5.1.2] Random jitter around a configured mean/variance.
- [REQ-5.1.3] Packet loss with configurable probability.
- [REQ-5.1.4] Packet duplication with configurable probability.
- [REQ-5.1.5] Packet reordering — buffer messages and release out of order.
- [REQ-5.1.6] Partitions / intermittent outages — drop or delay all traffic for configured intervals.

5.2 Configuration and control
- [REQ-5.2.1] Structured configuration object or file; no ad-hoc globals.
- [REQ-5.2.2] Enable/disable each impairment independently.
- [REQ-5.2.3] Configure parameters per channel or per peer where sensible.
- [REQ-5.2.4] Deterministic PRNG seed — production seed must come from cryptographically unpredictable source (getrandom() / /dev/urandom / hardware RNG); fixed or time-only seeds prohibited in production (HAZ-022, CWE-338; see SECURITY_ASSUMPTIONS.md §7).
- [REQ-5.2.5] Snapshot/log impairment decisions for debugging tests.
- [REQ-5.2.6] Entropy source failure is fatal in production — if getrandom() and /dev/urandom both fail, log FATAL and trigger reset handler; never continue with a weak seed (ALLOW_WEAK_PRNG_SEED must be undefined in production; HAZ-022, CWE-338).

5.3 Determinism and testability
- [REQ-5.3.1] Same seed + same inputs → same impairment sequence (reproducible).
- [REQ-5.3.2] Runs against LocalSimHarness without real sockets.

6. Platform/network backend requirements
Full prose specification and rationale: docs/TRANSPORT.md.

6.1 TCP (connection-oriented) backend
- [REQ-6.1.1] Standard 3-way handshake.
- [REQ-6.1.2] Configurable connection timeouts and retry logic.
- [REQ-6.1.3] Graceful disconnect (FIN/ACK); handle RST and abrupt disconnect.
- [REQ-6.1.4] Application-level flow control (queue limits).
- [REQ-6.1.5] Explicit length-prefixed framing over the TCP byte stream.
- [REQ-6.1.6] Correct partial read/write handling; one send != one receive.
- [REQ-6.1.7] Multiple simultaneous connections.
- [REQ-6.1.8] Client-side HELLO registration — TCP/TLS client must send HELLO frame (MessageType::HELLO=4, source_id=local NodeId, payload_length=0) before any DATA frame. Server builds NodeId->socket-slot routing table from received HELLOs.
- [REQ-6.1.9] Server-side unicast routing — maintain NodeId->slot table; route to matching slot for destination_id != NODE_ID_INVALID; ERR_INVALID + WARNING_HI if no slot found; fan-out to all for destination_id == 0 (broadcast).
- [REQ-6.1.10] TransportInterface::register_local_id(NodeId) — non-pure virtual, default returns OK. TCP/TLS client: send HELLO on-wire. TCP/TLS server: store local NodeId. UDP/DTLS: store NodeId and send HELLO datagram. DeliveryEngine::init() calls this after transport->init() returns OK; failure logged at WARNING_HI.
- [REQ-6.1.11] TCP/TLS source address validation — verify socket slot matches source_id in envelope before passing to DeliveryEngine; discard + WARNING_HI on mismatch (prevents source_id spoofing; see SECURITY_ASSUMPTIONS.md §4).
- [REQ-6.1.12] HELLO timeout — per-slot accept timestamp; evict + close + WARNING_HI any slot that has not sent HELLO within hello_timeout_ms (default: recv_timeout_ms) (HAZ-023, CWE-400).

6.2 UDP (connectionless) backend
- [REQ-6.2.1] No inherent reliability; optional ACK/NAK + retry; handle loss gracefully.
- [REQ-6.2.2] Sequence numbers for ordering; detect and handle duplicates and reordering.
- [REQ-6.2.3] Respect UDP size limits; fragmentation/reassembly at message layer if needed.
- [REQ-6.2.4] Source address validation — verify datagram source matches source_id in envelope; discard + WARNING_HI on mismatch (UDP equivalent of REQ-6.1.11).
- [REQ-6.2.5] Reject wildcard peer_ip — init() must reject "0.0.0.0", "::", or empty string with ERR_INVALID + FATAL (HAZ-024, CWE-290).

6.3 Common socket requirements (TCP and UDP)
- [REQ-6.3.1] Bind to configurable IP/port; IPv4 required, IPv6 optional; use SO_REUSEADDR.
- [REQ-6.3.2] Classify errors as recoverable vs fatal; map to WARNING_LO / WARNING_HI / FATAL.
- [REQ-6.3.3] Read/write timeouts; heartbeat/keepalive for long-lived connections.
- [REQ-6.3.4] Clear extension point for TLS (TCP) and DTLS (UDP); swappable without changing higher layers.
- [REQ-6.3.5] Explicit, configurable buffer sizes; no unbounded buffers.
- [REQ-6.3.6] CA cert required when verify_peer=true — init() returns ERR_IO + FATAL if ca_file is empty (HAZ-020, CWE-295).
- [REQ-6.3.7] CRL enforcement when require_crl=true — init() returns ERR_INVALID + FATAL if verify_peer=true and crl_file is empty; default require_crl=false.
- [REQ-6.3.8] Forward secrecy — when tls_require_forward_secrecy=true, reject TLS 1.2 session resumption by zeroizing session and returning ERR_IO; default false.
- [REQ-6.3.9] Hostname/peer-verify mismatch — init() returns ERR_INVALID + WARNING_HI if verify_peer=false and peer_hostname is non-empty (HAZ-025, CWE-297).
- [REQ-6.3.10] TlsSessionStore mutex — protect mbedtls_ssl_session struct with POSIX mutex against concurrent zeroize()/try_save/try_load; std::atomic alone is insufficient (HAZ-021, CWE-362).

6.4 DTLS (Datagram TLS over UDP) backend
- [REQ-6.4.1] DTLS transport mode (MBEDTLS_SSL_TRANSPORT_DATAGRAM); client and server roles.
- [REQ-6.4.2] DTLS cookie exchange on server — prevent amplification; bind cookie to client (address, port).
- [REQ-6.4.3] DTLS retransmission timer — configure min/max to match link RTT; mbedTLS timer callbacks; no busy-wait.
- [REQ-6.4.4] MTU enforcement — reject outbound messages exceeding DTLS_MAX_DATAGRAM_BYTES.
- [REQ-6.4.5] Plaintext fallback — when tls_enabled==false, use plain UDP socket path; no handshake.
- [REQ-6.4.6] DTLS peer hostname verification — when verify_peer==true and peer_hostname non-empty, call mbedtls_ssl_set_hostname() before handshake; return ERR_IO on failure; pass nullptr when peer_hostname is empty (explicit opt-out).

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
  - [REQ-7.2.5] Bounded observability event ring: a fixed-capacity, non-blocking
    ring buffer (poll_event / drain_events / pending_event_count) that records
    per-message delivery events (SEND_OK, SEND_FAIL, ACK_RECEIVED, ACK_TIMEOUT,
    DUPLICATE_DROP, EXPIRY_DROP, RETRY_FIRED, MISROUTE_DROP) without dynamic
    allocation, enabling post-hoc observability and test verification.

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

8.3 Project-specific security rule applicability (.claude/CLAUDE.md §7c–§7d)
The global security rules §7c and §7d apply to the following project components:

§7c — Secure zeroing of cryptographic material:
  Applies to: all key-loading paths in TlsTcpBackend (src/platform/TlsTcpBackend.cpp) and
  DtlsUdpBackend (src/platform/DtlsUdpBackend.cpp), and any future code that handles
  TLS/DTLS credentials or session tokens.
  Also applies to: TlsSessionStore::zeroize() (src/platform/TlsSessionStore.cpp) —
  zeroizes TLS session material (master-secret-derived data) before the
  TlsSessionStore struct goes out of scope or is reused (HAZ-017, CWE-316).

§7d — Timing-safe comparisons:
  Currently applies to: the DTLS cookie exchange path in DtlsUdpBackend (REQ-6.4.2,
  src/platform/DtlsUdpBackend.cpp). Extend to any future MAC/HMAC validation path.

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
docs/DEFECT_LOG.md. Full procedure, reviewer roles, entry/exit criteria, and severity
definitions: docs/INSPECTION_CHECKLIST.md.

Entry criteria summary: make + make lint + make check_traceability + make run_tests all pass;
all Implements/Verifies tags present; no raw assert() in src/.
Exit criteria summary: all CRITICAL and MAJOR defects dispositioned; moderator signed off in docs/DEFECT_LOG.md.

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

Security assumptions and invariants: docs/SECURITY_ASSUMPTIONS.md

---

14. Coverage Requirements (NASA-STD-8739.8A / NPR 7150.2D)

```xml
<coverage_requirements>
1. Mandatory floor — all SC functions (docs/HAZARD_ANALYSIS.md §3):
   - Branch coverage (true/false for every decision) is the pre-merge human review gate minimum.
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
Current worst case: 11 frames (Chain 3 — retry pump with send_fragments); ~130 KB stack
dominated by DtlsUdpBackend/TlsTcpBackend delayed[] buffer (flush path). Non-flush
worst case: ~764 bytes. Platform headroom: >10 000× on macOS/Linux (non-DTLS flush path).

Update trigger: update docs/STACK_ANALYSIS.md when any function introduces a
stack-allocated buffer >256 bytes, a new call chain exceeds 10 frames, or a new
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

2. Current verification baseline (per docs/VERIFICATION_POLICY.md VVP-001 §4.1):
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
