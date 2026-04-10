# Security Assumptions — messageEngine

This document records the security assumptions and invariants that the
messageEngine library relies on but cannot enforce by itself. Violating
any assumption listed here is a contract violation; the library's behaviour
is undefined when an assumption is broken.

Maintainers must review this document whenever a new component is added or
an existing component's public API changes.

---

## 1. Monotonic time contract

**Assumption:** Every caller of the following functions must supply a
`now_us` value derived from a monotonic clock source (POSIX `CLOCK_MONOTONIC`
or equivalent). The value must be non-decreasing across successive calls from
the same thread.

Affected functions:
- `DeliveryEngine::sweep_ack_timeouts(now_us)`
- `DeliveryEngine::pump_retries(now_us)`
- `DeliveryEngine::receive(out, timeout_ms, now_us)`
- `DeliveryEngine::send(envelope, now_us)`
- `AckTracker::sweep_expired(now_us)` — enforced by `NEVER_COMPILED_OUT_ASSERT(now_us >= m_last_sweep_us)`
- `RetryManager::collect_due(now_us, ...)` — enforced by `NEVER_COMPILED_OUT_ASSERT(now_us >= m_last_collect_us)`
- `ReassemblyBuffer::ingest(frag, out, now_us)`
- `ReassemblyBuffer::sweep_stale(now_us, threshold)`

**Enforcement (updated G-1 / SEC-007):**

- `AckTracker::sweep_expired()` and `RetryManager::collect_due()` previously asserted
  `NEVER_COMPILED_OUT_ASSERT(now_us >= m_last_sweep_us)`, which triggered `abort()` on
  any backward timestamp. As of commit d7584dd (G-1), this assert was replaced with a
  **WARNING_HI + clamp**: `now_us` is silently clamped to `m_last_sweep_us` and
  processing continues. The process degrades gracefully (slots may expire one sweep
  late) rather than crashing. The `now_us != 0ULL` precondition assert is retained.

- `DeliveryEngine` now enforces the monotonic contract at its own public API boundary
  (SEC-007, commit 3d58a1c). `send()`, `receive()`, `pump_retries()`, and
  `sweep_ack_timeouts()` each check `now_us >= m_last_now_us` on entry; a backward
  timestamp logs WARNING_HI and returns `ERR_INVALID` immediately. This catches
  violations before they propagate to inner components.

- `ReassemblyBuffer` and `OrderingBuffer` log `WARNING_HI` on backward timestamps and
  may fail to sweep stale slots on the affected cycle.

**Summary by component:**

| Component | Backward-timestamp behavior |
|---|---|
| `AckTracker::sweep_expired()` | WARNING_HI + clamp; degraded (slot expiry deferred by one sweep) |
| `RetryManager::collect_due()` | WARNING_HI + clamp; degraded (retry deferred by one sweep) |
| `DeliveryEngine` (public API) | WARNING_HI + ERR_INVALID; call rejected at API boundary |
| `ReassemblyBuffer::sweep_stale()` | WARNING_HI; stale slot not reclaimed until next valid timestamp |
| `OrderingBuffer` | WARNING_HI; held message not advanced on that sweep |

**Risk if violated:** Pending ACK and retry slots will not expire, permanently
exhausting fixed-capacity tables (`ACK_TRACKER_CAPACITY` = 32 slots,
`RETRY_MANAGER_CAPACITY` = 32 slots) until the component is re-initialized.

---

## 2. Unique message IDs per sender per session

**Assumption:** Within a single session (between two calls to `DeliveryEngine::init()`),
each `source_id` must produce strictly unique `message_id` values. The
`MessageIdGen` component guarantees this for locally-generated messages via
a monotonic counter; remote peers are assumed to provide the same guarantee.

**Risk if violated:** The duplicate filter (`DuplicateFilter`, window size =
`DEDUP_WINDOW_SIZE` = 128 entries) keys on `(source_id, message_id)`. ID
collision causes legitimate messages to be silently dropped as duplicates on
`RELIABLE_RETRY` paths.

**Seed entropy (S2 hardening):** Prior to commit 77c7106, `MessageIdGen` was initialized with `local_id` as the sole seed, making the first message ID of each session predictable to any party that knows the node ID. As of commit 77c7106, `DeliveryEngine::init()` XORs `local_id << 32` with `timestamp_now_us()` to add runtime entropy. This reduces the attack surface to an attacker who has both the node ID and sub-microsecond knowledge of the process start time. For higher assurance, embed the seed in a `getrandom()`-sourced value (see §7 for the PRNG seed policy).

---

## 3. Constant peer node IDs per session

**Assumption:** `source_id` and `destination_id` values must remain constant
for the duration of a logical session. Changing node IDs mid-session bypasses:
- Duplicate suppression (`DuplicateFilter` — keyed on `source_id`)
- Ordering tracking (`OrderingBuffer` — peer table keyed on `source_id`)
- ACK correlation (`AckTracker` — keyed on `source_id + message_id`)

**Risk if violated:** A peer that changes its `source_id` mid-session will
appear as a new peer, consuming new slots in all fixed-capacity tables and
potentially exhausting them.

**Reconnect handling (REQ-3.3.6):** A legitimate peer that disconnects and
reconnects is treated as starting a new session. `DeliveryEngine::drain_hello_reconnects()`
detects the reconnect via the HELLO frame queued by the backend's
`pop_hello_peer()` and calls `reset_peer_ordering(src)`, which resets
`next_expected_seq` to 1 and frees held slots for that peer. This is the only
sanctioned path for a peer's sequence counter to move backward (HAZ-016 mitigation).

---

## 4. Transport-layer source address validation

**Assumption:** The library does not validate the wire-level network source
address against the `source_id` field in the message envelope. Transport
backends are responsible for this validation per the requirements below:

- TCP/TLS backends: REQ-6.1.11 — validate socket slot vs. envelope source_id.
- UDP/DTLS backends: REQ-6.2.4 — validate datagram source address vs. envelope source_id.

Each backend must silently discard any envelope whose wire-level source does not
match the claimed source_id and log a WARNING_HI before the envelope reaches
`DeliveryEngine::receive()`.

REQ-6.1.11 is now enforced:

- **Server mode (TcpBackend / TlsTcpBackend):** `validate_source_id()` checks the inbound
  envelope's `source_id` against the NodeId registered via HELLO for that connection slot
  before passing any frame to `DeliveryEngine`. A mismatch results in silent discard and
  WARNING_HI. Cross-slot duplicate NodeId is also rejected: `handle_hello_frame()` scans
  all active slots and evicts the newcomer if the NodeId is already registered (SEC-012 /
  DEF-018-4 for TlsTcpBackend; G-3 + F-13 for TcpBackend).

- **Client mode (TlsTcpBackend):** After the TLS handshake, the first non-invalid
  `source_id` received from the server is locked in (SEC-011 / DEF-018-3). Subsequent
  frames from the server with a different `source_id` are logged at WARNING_HI and dropped.
  This prevents mid-session source_id substitution on the client side.

- **Client mode (TcpBackend — plaintext TCP):** The first non-invalid `source_id`
  received from the server is locked in to `m_client_node_ids[0U]` (SEC-025). Subsequent
  frames from the server fd with a different `source_id` are rejected by
  `validate_source_id()` with WARNING_HI and discarded. Mirrors TlsTcpBackend SEC-011.
  Prior to this fix, the client-mode slot remained `NODE_ID_INVALID` for the session
  lifetime, allowing an attacker with access to the TCP stream to exhaust `DuplicateFilter`
  and `OrderingBuffer` capacity by rotating arbitrary `source_id` values (HAZ-009).

REQ-6.2.4 is now enforced:

- **DtlsUdpBackend:** `process_hello_or_validate()` requires a HELLO frame before any
  DATA frame. The first HELLO locks in `m_peer_node_id`; subsequent data frames whose
  `source_id` does not match the registered NodeId are silently discarded with WARNING_HI.
  Duplicate HELLO frames are also rejected (HAZ-011 / DEF-015-3).

  **SEC-027 (two-phase port-locking):** Prior to this fix, `validate_source()` locked
  `m_peer_src_port` on the *first raw datagram* from the trusted IP, before
  deserialization or HELLO validation. A malformed packet, or a DATA-before-HELLO packet
  from the right host, could poison the locked port and cause a legitimate client arriving
  on a different ephemeral port to be permanently dropped. Fix: `validate_source()` now
  only records a *candidate* port in `m_pending_src_port`; `commit_pending_src_port()` is
  called from `process_hello_or_validate()` and moves the candidate to `m_peer_src_port`
  only after a valid HELLO frame is confirmed. Garbage or pre-HELLO datagrams leave
  `m_peer_src_port` at zero so the next datagram from any port can still produce a HELLO.

  **SEC-026 (bidirectional HELLO):** When the server processes the first client HELLO,
  `process_hello_or_validate()` immediately sends a HELLO response back to the client via
  `send_hello_datagram()`. This allows the client's own `m_peer_hello_received` guard to
  be set, enabling server→client DATA delivery. Without this response, the client dropped
  all server-originated DATA because the server never sent HELLO on `register_local_id()`.
  In plaintext mode the server uses `m_peer_src_port` (the client's learned ephemeral port)
  as the destination port for its reply, since `m_cfg.peer_port` is the server's own listen
  port (SEC-026 / SEC-023 port-learning fix in `send_wire_bytes()`).

- **UdpBackend (plaintext UDP):** `process_hello_or_validate()` mirrors the DtlsUdpBackend
  logic. A peer at the correct IP:port must send a HELLO before any DATA frame; the HELLO
  locks `m_peer_node_id`; subsequent data frames with a mismatched `source_id` are silently
  discarded with WARNING_HI. This closes the source_id rotation attack on the plaintext path
  (REQ-6.2.4 / REQ-6.1.8). Prior to this fix, `validate_source()` checked IP:port only —
  a peer at the right address could exhaust `DuplicateFilter` and `OrderingBuffer` capacity
  by cycling through arbitrary `source_id` values.

**Invariant:** All four transport backends (TcpBackend, TlsTcpBackend, DtlsUdpBackend,
UdpBackend) now enforce HELLO-before-data and source_id binding before any envelope
reaches `DeliveryEngine::receive()`.

**Risk if violated:** A peer that spoofs another peer's `source_id` can inject
messages that are processed as if from the legitimate peer, potentially
corrupting ordering state or triggering spurious ACK/duplicate handling.

---

## 5. Sequence number practical limit

**Assumption:** `sequence_num` is a `uint32_t`. The `OrderingBuffer` advances
`next_expected_seq` via `advance_next_expected()`, which asserts:
```
NEVER_COMPILED_OUT_ASSERT(next_expected_seq <= 0x7FFFFFFFU)
```
A session that exhausts 2^31 sequence numbers without a reset will trigger
a FATAL. At 1,000,000 messages/second, this limit is reached in approximately
35 minutes. Sessions are expected to reset (re-init) far sooner in practice.
On peer reconnect, `reset_peer()` resets `next_expected_seq` to 1U, so the
sequence counter restarts from scratch for the new session (REQ-3.3.6).

**Action required if long-running sessions are needed:** Widen `sequence_num`
to `uint64_t` and update the assertion ceiling before deploying to any context
where a single session may deliver more than ~2 billion ordered messages.

On wraparound at `0xFFFFFFFF`, `advance_next_expected()` logs `WARNING_HI`
and resets `next_expected_seq` to `1U` rather than crashing, giving the
application a chance to detect and handle the condition.

---

## 6. Reassembly slot exhaustion window

**Assumption:** Multi-fragment messages that stall mid-send (after some but
not all fragments reach the receiver, i.e., `ERR_IO_PARTIAL` from the sender)
will hold a `ReassemblyBuffer` slot for up to `recv_timeout_ms` milliseconds
before `sweep_stale()` reclaims it.

With `REASSEMBLY_SLOT_COUNT` = 8 slots, a peer that repeatedly sends only the
first fragment of distinct multi-fragment messages can delay reassembly for
other peers for up to `recv_timeout_ms` per occupied slot. The `sweep_stale()`
mechanism (REQ-3.2.9) mitigates but does not fully eliminate this window.

**Mitigation already in place:**
- `DeliveryEngine::sweep_ack_timeouts()` calls `sweep_stale()` each sweep cycle.
- Stale slot expiry is logged at `WARNING_LO` for observability.

**Residual risk:** If `recv_timeout_ms` is set very large (e.g., minutes),
the exhaustion window is correspondingly large. Keep `recv_timeout_ms`
proportional to actual network RTT to limit exposure.

---

## 7. PRNG seed and impairment determinism

**Assumption:** The impairment engine uses a seedable PRNG (REQ-5.3.1). If
two engines are seeded identically and receive the same message sequence, they
produce identical impairment decisions. This is intentional for deterministic
testing but means that an attacker who knows the seed and the message sequence
can predict exactly which messages will be dropped or delayed.

**Normative requirement (REQ-5.2.4):** In production builds the PRNG seed must
be derived from a cryptographically unpredictable source (e.g., `getrandom()`,
`/dev/urandom`, or a hardware RNG). A fixed literal or time-only seed is
prohibited in production deployments. The seed source must be documented in
deployment configuration. Do not use a fixed or guessable seed when impairments
are active in a production environment.

---

## §8 — DTLS peer certificate hostname validation

When `TlsConfig::verify_peer` is true and `TlsConfig::peer_hostname` is non-empty,
the DTLS client validates that the server certificate's CN or SAN matches
`peer_hostname` via `mbedtls_ssl_set_hostname()` (REQ-6.4.6). Without this binding,
CA-chain validation alone does not prevent impersonation by any certificate from
the same trusted CA (CWE-297 / MitM). The `DtlsUdpBackend` client path is required
to call `ssl_set_hostname` after `ssl_setup` and treat any non-zero return as fatal
(returns `ERR_IO`). See HAZ-008 in docs/HAZARD_ANALYSIS.md.

---

## §11 TLS TCP peer certificate hostname validation (SEC-021 — mirrors §8 for TCP)

When `TlsConfig::verify_peer` is true and `TlsConfig::peer_hostname` is non-empty,
the TLS TCP client validates that the server certificate's CN or SAN matches
`peer_hostname` via `mbedtls_ssl_set_hostname()`. Without this binding, CA-chain
validation alone does not prevent impersonation by any certificate from the same
trusted CA (CWE-297 / MitM).

**Enforcement (SEC-021, commit 4cda101):**
`tls_connect_handshake()` in `TlsTcpBackend` rejects `verify_peer=true` with an empty
`peer_hostname` before the handshake begins: logs WARNING_HI and returns `ERR_INVALID`.
This mirrors the DtlsUdpBackend SEC-001 fix documented in §8 above and closes the
same CWE-297 gap for the TLS TCP transport. See HAZ-008 in docs/HAZARD_ANALYSIS.md.

**Invariant:** Both TLS-capable backends (TlsTcpBackend and DtlsUdpBackend) now enforce
the rule: `verify_peer=true` with an empty `peer_hostname` is a configuration error
that must be rejected before the handshake is attempted.

---

## §9 Non-constant-time dedup scan (timing side-channel, low risk)

`DuplicateFilter::is_duplicate()` scans the dedup window linearly and returns
early on the first matching `(source_id, message_id)` pair. The loop exit time
therefore correlates with whether and where a match exists in the window, creating
a timing side-channel that could theoretically allow an observer to infer whether a
given message was seen recently.

**Risk assessment — LOW**: the window is at most `DEDUP_WINDOW_SIZE` (128) entries;
the timing delta is on the order of tens of nanoseconds; and all production transports
run over TLS/DTLS, whose record-layer padding and MAC computation dominate any
variation the dedup scan adds. An attacker who can make precise sub-microsecond
timing measurements through TLS encryption has capabilities far beyond those modelled
here.

**Decision**: no code change. Document the side-channel here so future reviewers can
re-evaluate if the transport assumptions change (e.g., plaintext UDP deployment).

---

## §10 Sequential message ID predictability

`MessageIdGen::next_id()` returns a monotonically increasing counter seeded at
initialization from a cryptographically unpredictable source (`arc4random_buf` on
macOS, `getrandom` on Linux with `/dev/urandom` fallback). After observing two
consecutive message IDs from a node, an attacker can recover the full counter state
and predict all future IDs from that node for the lifetime of the process.

**Scope of impact**: message IDs are used only for deduplication
(`DuplicateFilter`), not for authentication, authorization, or integrity. An
attacker who can predict future IDs cannot forge accepted messages (that requires
bypassing TLS/DTLS) and cannot suppress legitimate messages (dedup only drops
repeated IDs, not absent ones). The predictability affects only the collision
probability of the dedup window under adversarial traffic.

**Risk assessment — LOW for current deployment (TLS/DTLS protected transports)**.
Becomes MEDIUM if the system is deployed over plaintext UDP (`tls_enabled = false`)
and an attacker can both observe traffic and inject spoofed datagrams (source
validation per REQ-6.2.4 is the primary mitigant in that scenario).

**Mitigations in place**:
- REQ-6.4.2 DTLS cookie anti-replay prevents amplification-based ID harvesting.
- REQ-6.1.11 / REQ-6.2.4 source-address validation blocks spoofed-source injection.
- Production seed requirement (REQ-5.2.4) prohibits fixed or time-only seeds.

**Decision**: no code change. Document here and in the deployment configuration
guide. Re-evaluate if the system is deployed on a low-latency plaintext transport.

---

## §13 TLS session store lifetime contract (HAZ-017)

`TlsSessionStore` is a caller-owned value type that holds a TLS session snapshot
(including master-secret-derived material) between a `TlsTcpBackend::close()` call
and the next `init()` call on the same backend instance.  This design allows
abbreviated TLS session resumption (RFC 5077) across reconnect cycles without
requiring dynamic allocation on the critical path (Power of 10 Rule 3).

**Caller responsibilities:**

1. **Call `store.zeroize()` before scope exit** — The store retains active session
   material (including master-secret-derived ticket key data) from the moment
   `try_save_client_session()` sets `session_valid = true` until `zeroize()` is
   explicitly invoked.  The caller must call `zeroize()` before:
   - The `TlsSessionStore` goes out of scope (stack frame or heap free).
   - The process terminates normally.
   - Any point where the memory may be read externally (core dump, swap file,
     hibernation image, heap-spray read).

2. **Do not serialize or pass the store across process boundaries** — The
   `mbedtls_ssl_session` struct contains internal pointers that are valid only
   within the originating process.  Copying the raw bytes to another process or
   persisting them to disk is undefined behaviour and may expose key material.

3. **Do not copy or move the store** — The copy and move constructors and
   assignment operators are `= delete`.  Any attempt to copy or move a
   `TlsSessionStore` is a compile-time error.  This enforces the mbedTLS
   constraint that `mbedtls_ssl_session` cannot be bitwise-copied (it may hold
   internal heap allocations that cannot be aliased).

**Library-provided safety nets (informational — not a substitute for rule 1):**

- `TlsSessionStore::~TlsSessionStore()` calls `zeroize()` automatically.  This
  provides a last-resort cleanup if the caller forgets the explicit call, but
  relies on the destructor running before the memory is read externally.  Do not
  rely on this as the primary cleanup path.
- `TlsTcpBackend::close()` logs `WARNING_LO` when `store.session_valid == true`
  after close, reminding the caller that session material is still live.
- `TlsSessionStore::zeroize()` uses `mbedtls_platform_zeroize()` (not `memset`)
  to guarantee the compiler cannot elide the zeroing operation (CWE-14,
  CLAUDE.md §7c).

**TLS 1.2 forward-secrecy limitation:**
When a TLS 1.2 session is resumed via this store, the abbreviated handshake reuses
the original session's key material; no new ephemeral Diffie-Hellman exchange
occurs.  This means that if the original session's key material is compromised, all
traffic from the resumed session is also compromised (no forward secrecy for
resumed sessions under TLS 1.2 — RFC 5077 §5).  `try_save_client_session()` logs
a one-time `WARNING_LO` on the first TLS 1.2 session save to alert the operator.
TLS 1.3 session resumption uses PSK-based 0-RTT or 1-RTT mechanisms that preserve
forward secrecy; this limitation does not apply when TLS 1.3 is negotiated.

**Hazard coverage:** HAZ-017 (CWE-316 — session material in caller memory after
close); extension of HAZ-012 (CWE-14 — private key in freed memory).

---

## References

- `docs/HAZARD_ANALYSIS.md` — hazard catalogue and SC function classification
- `docs/VERIFICATION_POLICY.md` — VVP-001 verification methods
- `CLAUDE.md §13` — Software Safety Requirements
- `CONTRIBUTING.md` — security vulnerability reporting process (`SECURITY.md`)
