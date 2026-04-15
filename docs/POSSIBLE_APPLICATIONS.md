# Possible Applications

Applications that are a good fit for messageEngine share a common profile: they need reliable
messaging over unreliable links, bounded memory, deterministic behavior under test, and/or
the ability to inject realistic network faults without a real broken network.

---

## Aerospace / Embedded Systems

- **Satellite ground station command & telemetry relay** — reliable-retry delivery with expiry
  ensures stale commands are never executed; DTLS/UDP backend suits constrained RF links
- **Drone swarm coordination** — tolerates link loss and reordering; ordered channels keep
  control frames sequenced while telemetry can use best-effort
- **Avionics data bus simulator for hardware-in-the-loop testing** — `LocalSimHarness` runs
  without OS sockets; seedable PRNG reproduces fault scenarios exactly
- **CubeSat onboard messaging between subsystems** — no heap after init; MISRA C++:2023 and
  Power of 10 compliance is a strong head start toward flight software certification
- **Remote firmware update over RF** — fragment large firmware images across 4 KB frames using
  the fragmentation layer; reliable-retry with expiry guarantees all fragments arrive before a
  maintenance window closes, and expired partial updates are discarded cleanly
- **RTOS task IPC simulation layer** — use `LocalSimHarness` as an in-process message bus
  between tasks in a POSIX simulation of an RTOS; validate message flows and fault handling
  before porting IPC to bare-metal shared-memory or mailbox primitives
- **Embedded health monitor / watchdog messaging** — subsystems send periodic heartbeats using
  `RELIABLE_ACK`; a missing ACK within the deadline detects silent process death or bus lockup
  without polling loops or shared-memory flags
- **Hardware-in-the-loop fault replay** — record the PRNG seed and impairment config that
  triggered a field failure; replay the exact fault sequence against hardware under test to
  reproduce intermittent bugs without physical RF interference equipment
- **Edge gateway sensor aggregation** — high-rate sensor readings use `BEST_EFFORT` channels to
  avoid backpressure; critical alarms use `RELIABLE_RETRY` with short expiry; the observability
  ring tracks per-channel drop rates for field diagnostics without dynamic allocation

---

## Distributed Systems Testing

- **Chaos engineering harness** — inject realistic latency, jitter, loss, duplication, and
  partitions into microservice integration tests without a real broken network
- **Protocol conformance test bench** — verify a client/server pair behaves correctly under
  partition, duplication, and reordering by swapping in `LocalSimHarness` + `ImpairmentEngine`
- **CI network fault injection** — wrap any TCP service in `LocalSimHarness` to reproduce
  flaky-network bugs deterministically in every CI run
- **Distributed algorithm test harness** — controllable network layer underneath Raft, Paxos,
  or CRDT implementations; inject split-brain and message-loss scenarios on demand

---

## Industrial / Safety-Critical

- **Factory floor PLC-to-SCADA messaging** — reliable-retry with expiry; observability ring
  provides post-hoc audit trail without dynamic allocation
- **Medical device inter-subsystem communication** — bounded allocation, no heap on critical
  paths, and MISRA compliance are prerequisites in IEC 62443 / IEC 62304 contexts
- **Autonomous vehicle sensor fusion bus** — ordered delivery for safety-critical channels,
  best-effort for high-rate sensor streams, observability counters for health monitoring

---

## Simulation & Wargaming

- **Multiplayer game state synchronization over unreliable links** — per-channel reliability
  mode lets position updates use best-effort while game events use reliable-retry
- **Military communications simulator** — model contested RF links with configurable partition
  and jitter profiles driven by a reproducible PRNG seed
- **Network emulator middleware** — sit between two real applications and apply impairments
  to characterize how the application degrades under realistic WAN conditions

---

## Developer Tooling

- **Fuzz test harness for network protocol implementations** — feed malformed, reordered, and
  duplicated frames via `LocalSimHarness` to stress-test a protocol stack under controlled chaos
- **Embedded systems bring-up test fixture** — validate that a new platform port handles
  partial reads, connection resets, and timeout paths before connecting real hardware

---

## Poor Fits

The following use cases are **not well suited** to messageEngine:

| Scenario | Why it does not fit |
|---|---|
| High-throughput bulk data transfer | 4 096-byte payload cap and 8-connection limit are constraining for large file or stream transfer |
| Dynamic peer discovery | `MAX_PEER_NODES` is a compile-time constant; runtime peer registration is not supported |
| Browser or mobile clients | No JavaScript/Kotlin/Swift bindings; C++17 with POSIX sockets only |
| Pub/sub fan-out at scale | No built-in topic routing; broadcast is limited to all connected clients |
