# messageEngine Use Case Index

Actors: **User** (application / developer) | **System** (messageEngine — grey box)

---

## HL-1: User sends a fire-and-forget message
> User sends a fire-and-forget message; System delivers it to the network with no acknowledgement or tracking.

- UC_01 — Best-effort send over TCP

---

## HL-2: User sends a message requiring confirmation
> User sends a message requiring confirmation; System transmits it and notifies User whether the remote peer acknowledged receipt.

- UC_02 — Reliable-with-ACK send
- UC_08 — ACK resolution (completion event for HL-2: System resolves the pending tracker entry when the ACK arrives)

---

## HL-3: User sends a message requiring guaranteed delivery
> User sends a message requiring guaranteed delivery; System retransmits it automatically until acknowledged or expired, transparently deduplicating on the receiver side.

- UC_03 — Reliable-with-retry send with exponential backoff

---

## HL-4: User sends a message with a deadline
> User sends a message with a deadline; System discards it silently if the deadline passes before delivery.

- UC_04 — Expired message dropped on send path

---

## HL-5: User waits for an incoming message
> User waits for an incoming message; System returns the next available message, or discards it and returns ERR_EXPIRED if the message's deadline has passed before delivery.

- UC_09 — Expired message dropped on receive path

---

## HL-6: System suppresses duplicate messages
> User receives a message; System silently suppresses delivery if the message is a duplicate already seen in the sliding window.

- UC_07 — Duplicate message drop
- UC_33 — Dedup sliding-window eviction test

---

## HL-7: User starts a server endpoint
> User starts a server endpoint; System binds a listen socket, accepts incoming client connections, and stores their file descriptors.

- UC_19 — TCP server accept

---

## HL-8: User starts a client endpoint
> User starts a client endpoint; System establishes a TCP connection to the configured peer.

- UC_35 — TCP client connect

---

## HL-9: User closes a transport
> User closes a transport; System flushes pending messages and releases all socket resources.

- UC_34 — Transport teardown: TcpBackend::close(), DeliveryEngine shutdown, and in-flight message handling

---

## HL-10: User pumps the retry loop
> User pumps the retry loop; System retransmits any messages whose backoff interval has elapsed, doubling the interval on each attempt.

- UC_10 — RetryManager fires scheduled retry
- UC_12 — Retry cancelled on ACK receipt (termination path of the retry cycle)

---

## HL-11: User sweeps ACK timeouts
> User sweeps ACK timeouts; System identifies unacknowledged messages past their deadline and logs them as failures.

- UC_11 — AckTracker timeout sweep

---

## HL-12: User configures network impairments
> User configures network impairments (loss, latency, jitter, duplication, reordering, partition); System applies those faults to all traffic for simulation or testing.

- UC_13 — Impairment: packet loss
- UC_14 — Impairment: duplication
- UC_15 — Impairment: fixed latency
- UC_16 — Impairment: jitter
- UC_17 — Impairment: reordering
- UC_18 — Impairment: partition
- UC_32 — 100% loss configuration test

---

## HL-13: User seeds the impairment engine for deterministic replay
> User seeds the impairment engine with a fixed value; System produces a deterministic, reproducible fault sequence for test replay.

- UC_29 — PRNG deterministic seed
- UC_31 — PRNG reproducibility test

---

## HL-14: User links two in-process endpoints
> User links two in-process endpoints; System routes messages between them locally without any real network, enabling fully deterministic integration testing.

- UC_24 — LocalSimHarness in-process delivery
- UC_30 — LocalSimHarness round-trip test

---

## HL-15: User monitors the system
> User monitors the System; System emits structured log events (INFO / WARNING_LO / WARNING_HI / FATAL) for every significant state change, error, and message lifecycle event.

- No dedicated detailed UC document. Logging behavior is observable as a side effect across all other use cases.

---

## HL-16: User initializes the system
> User creates a transport and delivery engine with a configuration; System prepares all components and establishes the connection ready for operation.

- UC_27 — Config defaults and overrides
- UC_28 — DeliveryEngine initialization

---

## HL-17: User sends or receives over UDP
> User sends or receives a message over a connectionless transport; System handles datagram framing without TCP connection state.

- UC_22 — UDP send datagram
- UC_23 — UDP receive datagram

---

## Application Workflow (above system boundary)

These use cases document patterns that combine multiple system calls and sit at
the application layer rather than at the system boundary. They are not single
User → System interactions.

- UC_05 — Server echo reply — Application pattern: receive a DATA message (HL-5) then send a reply (HL-1/2/3). Spans the receive and send boundaries; not a single system capability.

---

## System Internals (sub-functions, not user-facing goals)

These use cases document mechanisms that are invoked internally by the System on
behalf of other use cases. The User never calls them directly; they are invisible
at the User → System boundary.

- UC_06 — TCP inbound deserialization — Internal implementation of HL-5 (receive). Framing and deserialization are hidden inside `receive_message()`.
- UC_20 — TCP send framed message — Internal implementation of HL-1/2/3 (send). Length-prefix framing is hidden inside `send_message()`.
- UC_21 — TCP poll and receive — Internal implementation of HL-5 (receive). Polling multiple file descriptors is a system detail.
- UC_25 — Serializer encode — Sub-function of every send path. No direct user-visible action.
- UC_26 — Serializer decode — Sub-function of every receive path. No direct user-visible action.
