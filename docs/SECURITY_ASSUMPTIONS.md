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

**Enforcement:** `AckTracker` and `RetryManager` assert monotonicity via
`NEVER_COMPILED_OUT_ASSERT` and will trigger a FATAL reset on violation.
Other components log `WARNING_HI` or exhibit degraded behaviour (e.g.,
stale reassembly slots not freed on the sweep cycle that receives a
backward timestamp).

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

REQ-6.1.11 is now enforced: `validate_source_id()` in `TcpBackend` and `TlsTcpBackend` checks the inbound envelope's `source_id` against the NodeId registered via HELLO for that connection slot before passing any frame to `DeliveryEngine`. A mismatch results in silent discard and WARNING_HI.

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

## References

- `docs/HAZARD_ANALYSIS.md` — hazard catalogue and SC function classification
- `docs/VERIFICATION_POLICY.md` — VVP-001 verification methods
- `CLAUDE.md §13` — Software Safety Requirements
- `CONTRIBUTING.md` — security vulnerability reporting process (`SECURITY.md`)
