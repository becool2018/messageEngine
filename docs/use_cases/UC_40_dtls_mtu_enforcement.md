# UC_40 — MTU enforcement: outbound message whose serialised size exceeds DTLS_MAX_DATAGRAM_BYTES is rejected with ERR_INVALID

**HL Group:** HL-19 — DTLS-Encrypted UDP Transport
**Actor:** User
**Requirement traceability:** REQ-4.1.2, REQ-6.4.4

---

## 1. Use Case Overview

**Trigger:** User calls `DtlsUdpBackend::send_message(envelope)` where the serialised wire size of `envelope` exceeds `DTLS_MAX_DATAGRAM_BYTES` (1400 bytes). File: `src/platform/DtlsUdpBackend.cpp`.

**Goal:** Prevent IP fragmentation by rejecting any outbound message whose serialised size would exceed the DTLS record MTU before any encryption attempt is made.

**Success outcome (of this UC — i.e., rejection):** `send_message()` returns `Result::ERR_INVALID`. No data is sent on the wire. `m_ssl` and `m_sock_fd` are unchanged.

**Error outcomes (from the perspective of the caller):**
- `Result::ERR_INVALID` — message serialised to more than `DTLS_MAX_DATAGRAM_BYTES` bytes.
- `Result::ERR_IO` — `Serializer::serialize()` itself fails (separate path, not this UC).

---

## 2. Entry Points

```cpp
// Safety-critical (SC): HAZ-005, HAZ-006 — verified to M5
Result DtlsUdpBackend::send_message(const MessageEnvelope& envelope)
    // src/platform/DtlsUdpBackend.cpp:613
```

---

## 3. End-to-End Control Flow

1. `DtlsUdpBackend::send_message(envelope)` called.
2. Assertions: `m_open`, `envelope_valid(envelope)`.
3. `uint32_t wire_len = 0U`.
4. `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` — serialises the envelope into `m_wire_buf`. If this returns non-OK (malformed envelope or buffer overflow): log WARNING_LO, return that result. On success: `wire_len` now holds the serialised byte count.
5. **MTU check:** `if (wire_len > DTLS_MAX_DATAGRAM_BYTES)` — `DTLS_MAX_DATAGRAM_BYTES = 1400`.
   - **True:** Log WARNING_HI "send_message: serialized len N exceeds DTLS MTU 1400; rejected". Return `Result::ERR_INVALID`. No further processing. No impairment engine call. No socket write. `m_ssl` untouched.
   - **False:** Continue to impairment engine and wire send.
6. (Not part of this UC) `timestamp_now_us()` → `m_impairment.process_outbound()` → `ssl_write()` or `send_to()`.

---

## 4. Call Tree

```
DtlsUdpBackend::send_message()
 ├── envelope_valid()                  [assertion]
 ├── Serializer::serialize()
 │    └── [manual big-endian encoding into m_wire_buf]
 └── [MTU check: wire_len > DTLS_MAX_DATAGRAM_BYTES]
      ├── True:  Logger::log(WARNING_HI) → return ERR_INVALID
      └── False: [continue — impairment + ssl_write/send_to]
```

---

## 5. Key Components Involved

- **`DtlsUdpBackend::send_message()`**: Performs the MTU check immediately after serialisation and before any encryption or network I/O.
- **`Serializer::serialize()`** (`src/core/Serializer.cpp`): Produces the wire representation in `m_wire_buf`. `WIRE_HEADER_SIZE=44` bytes + `envelope.payload_length` bytes = minimum wire size; max is `44 + MSG_MAX_PAYLOAD_BYTES (4096) = 4140` bytes — far exceeding the 1400-byte DTLS MTU for maximum-sized payloads.
- **`DTLS_MAX_DATAGRAM_BYTES`** (from `src/core/Types.hpp`): Compile-time constant = 1400. Chosen to fit within typical IPv4 MTU (1500) minus IP (20) + UDP (8) + DTLS record overhead (~72 bytes).
- **`Logger::log(WARNING_HI)`**: Emits an observable rejection event for diagnostics.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|---|---|---|
| `Serializer::serialize()` returns non-OK | Log, return that error | `wire_len` is valid |
| `wire_len > DTLS_MAX_DATAGRAM_BYTES (1400)` | Log WARNING_HI, return `ERR_INVALID` | Continue to impairment + send |

The MTU check is a hard rejection: there is no fragmentation or retry. The caller receives `ERR_INVALID` and must reduce the payload before retrying.

---

## 7. Concurrency / Threading Behavior

- `send_message()` executes entirely on the caller's thread with no locking.
- `m_wire_buf` is a member buffer; concurrent calls from multiple threads would race on `m_wire_buf`. The single-threaded usage model means this is not a concern in practice.
- No atomic operations occur on this path.

---

## 8. Memory & Ownership Semantics

- **`m_wire_buf[SOCKET_RECV_BUF_BYTES]`** (8192 bytes, member): `Serializer::serialize()` writes here. If the MTU check triggers, the buffer contains the serialised bytes but they are discarded — no cleanup needed.
- **Power of 10 Rule 3:** No heap allocation. `Serializer::serialize()` writes into the fixed member buffer.
- **Stack:** No significant stack allocations in this specific path beyond `wire_len (uint32_t)`.

---

## 9. Error Handling Flow

| Error condition | Result code | State | Caller action |
|---|---|---|---|
| `wire_len > DTLS_MAX_DATAGRAM_BYTES` | `ERR_INVALID` | Unchanged; nothing sent | Caller must reduce payload size |
| `Serializer::serialize()` fails | `ERR_IO` or `ERR_INVALID` | `wire_len == 0` | Caller should inspect envelope validity |

After `ERR_INVALID`, the system is fully consistent: no partial send occurred, no DTLS state was modified.

---

## 10. External Interactions

None — this flow terminates before any socket or TLS API call. The rejection happens entirely within process memory.

---

## 11. State Changes / Side Effects

| Object | Before | After `ERR_INVALID` return |
|---|---|---|
| `m_wire_buf` | arbitrary | contains serialised bytes (not sent) |
| `m_ssl` | established | unchanged |
| `m_sock_fd` | valid | unchanged |
| `m_open` | `true` | `true` |

The Logger emits one WARNING_HI entry recording the rejected message's wire length.

---

## 12. Sequence Diagram

```
User                 DtlsUdpBackend        Serializer      Logger
 |                        |                    |              |
 | send_message(envelope) |                    |              |
 |----------------------->|                    |              |
 |                        | serialize(envelope)|              |
 |                        |------------------> |              |
 |                        | <-- OK, wire_len=N |              |
 |                        |                    |              |
 |                        | [N > 1400?]        |              |
 |                        | YES                |              |
 |                        | log(WARNING_HI "serialized len N exceeds MTU") |
 |                        |---------------------------------------------->|
 | <-- ERR_INVALID -------|                    |              |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:** `DTLS_MAX_DATAGRAM_BYTES` is a compile-time constant (`src/core/Types.hpp`); `mbedtls_ssl_set_mtu()` is called during handshake to cap the DTLS record layer as well. The application-level MTU check in `send_message()` is a belt-and-suspenders guard that fires before mbedTLS is even invoked.

**Runtime:** Every call to `send_message()` performs this check unconditionally. There is no cached "previous wire length"; `Serializer::serialize()` runs on each call.

---

## 14. Known Risks / Observations

- **User transparency:** The MTU limit interacts with `MSG_MAX_PAYLOAD_BYTES = 4096`. A message with ~1357+ bytes of payload will exceed the 1400-byte MTU (`WIRE_HEADER_SIZE (44) + 1357 = 1401`). Users building messages near that payload size will unexpectedly receive `ERR_INVALID`.
- **DTLS overhead not pre-subtracted:** `DTLS_MAX_DATAGRAM_BYTES = 1400` is compared against the raw serialised size. The DTLS record overhead (~72 bytes for AES-128-GCM) is absorbed within the 1400-byte budget because `mbedtls_ssl_set_mtu()` was called with the same value — the TLS layer will further cap its output, but the application check is the first line of defence.
- **No fragmentation fallback:** Unlike UDP fragmentation (handled by the IP layer), there is no application-level message splitting. Oversized messages are permanently rejected.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `Serializer::serialize()` always produces `WIRE_HEADER_SIZE (44) + envelope.payload_length` bytes when successful. If the serializer applies compression or variable-length encoding in future, the MTU comparison would need updating.
- `[ASSUMPTION]` `DTLS_MAX_DATAGRAM_BYTES = 1400` was chosen to leave ~100 bytes of margin below a 1500-byte Ethernet MTU after IP (20) + UDP (8) headers. DTLS record overhead is absorbed within this margin. This is inferred from standard DTLS MTU guidelines, not from an explicit comment in the source.
