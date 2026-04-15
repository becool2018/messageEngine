# Impairment Engine Specification

**Normative REQ IDs:** defined in `CLAUDE.md §5`. This document contains the full prose specification and rationale. When a conflict exists, the REQ ID lines in `CLAUDE.md` take precedence.
**Last updated:** 2026-04-15

---

## Overview

The `ImpairmentEngine` (`src/platform/ImpairmentEngine.hpp`) sits logically between the transport abstraction and the underlying I/O backends. It intercepts outbound messages from `DeliveryEngine` before they reach the transport and intercepts inbound messages after they arrive, applying configured fault conditions to simulate realistic network behaviour.

The engine is usable with both real socket backends (`TcpBackend`, `UdpBackend`, `TlsTcpBackend`, `DtlsUdpBackend`) and the in-process simulation harness (`LocalSimHarness`).

---

## 5.1 Impairment Types

### REQ-5.1.1 — Fixed Latency
Every outbound message is held in a delay buffer for a configured `fixed_latency_ms`. Messages are released in FIFO order once the delay elapses. The delay buffer is a fixed-capacity array of `IMPAIR_DELAY_BUF_SIZE` slots.

### REQ-5.1.2 — Random Jitter
A per-message delay is drawn from a uniform distribution `[0, jitter_max_ms]` and added on top of any fixed latency. The delay is computed by `PrngEngine::next_range()` at the time the message enters the delay buffer.

### REQ-5.1.3 — Packet Loss
Each outbound message is dropped with probability `loss_probability` (range 0.0–1.0). The loss decision is made by `PrngEngine::next_double()` before any other impairments are applied. A dropped message is never inserted into the delay buffer.

### REQ-5.1.4 — Packet Duplication
Each outbound message that survives the loss check is duplicated with probability `dup_probability`. The duplicate is inserted into the delay buffer with a short additional delay (`dup_delay_ms`) to mimic network path asymmetry.

### REQ-5.1.5 — Packet Reordering
Inbound messages are optionally held and released out of sequence. `reorder_probability` controls how often a message is held rather than delivered immediately. Held messages are released after `reorder_delay_ms` elapses. The delay buffer is shared with fixed-latency messages.

### REQ-5.1.6 — Partitions / Intermittent Outages
A partition drops all outbound traffic for a configured interval (`partition_duration_ms`) and then resumes normal operation after a quiet period (`partition_gap_ms`). While a partition is active, `process_outbound()` drops every message without inserting it into the delay buffer. The partition state machine has three states: `IDLE`, `PARTITIONED`, `RECOVERING`. State transitions are logged at `INFO`.

---

## 5.2 Configuration and Control

### REQ-5.2.1 — Structured Configuration Object
All impairment parameters are grouped in `ImpairmentConfig` (`src/platform/ImpairmentEngine.hpp`). No global variables or ad-hoc parameter passing. Configuration is loaded from a file via `impairment_config_load()` or set programmatically.

### REQ-5.2.2 — Independent Enable/Disable
Each impairment type has an independent enable flag (`loss_enabled`, `jitter_enabled`, `dup_enabled`, `reorder_enabled`, `partition_enabled`). Enabling one impairment does not affect others.

### REQ-5.2.3 — Per-Channel or Per-Peer Configuration
`ImpairmentConfig` can be attached to a specific channel via `ChannelConfig::impairment` or applied globally. Per-peer impairment is supported by instantiating separate `ImpairmentEngine` instances keyed by peer `NodeId` in `DeliveryEngine`.

### REQ-5.2.4 — Deterministic PRNG Seed
The `PrngEngine` (`src/platform/PrngEngine.hpp`) implements xorshift64. In test builds the seed is set explicitly via `PrngEngine::seed(value)` before any impairment decisions are made. In production builds, the seed must be derived from a cryptographically unpredictable source:
- Preferred: `getrandom()` (Linux 3.17+, macOS 10.12+)
- Fallback: `/dev/urandom`
- Hardware RNG: acceptable if available and documented in deployment configuration

Fixed literal seeds (`seed(42)`) and time-only seeds (`seed(time(nullptr))`) are prohibited in production builds. Rationale: a known seed combined with a known message sequence allows an adversary to predict all impairment decisions, potentially enabling timing attacks. See `docs/SECURITY_ASSUMPTIONS.md §7`.

### REQ-5.2.5 — Snapshot / Log Impairment Decisions
`ImpairmentEngine` logs every impairment decision at `DEBUG` level: the message ID, the impairment type applied, and the PRNG state used. This enables post-hoc reconstruction of the fault sequence during test debugging without rerunning the full scenario.

### REQ-5.2.6 — Entropy Source Failure is Fatal in Production
In production builds (`ALLOW_WEAK_PRNG_SEED` undefined), if both `getrandom()` and `/dev/urandom` fail, the seed path logs `FATAL` and triggers `IResetHandler::on_fatal_assert()` via `NEVER_COMPILED_OUT_ASSERT`. Continuing with a weak seed (e.g., zero, timestamp-only, or a fixed constant) is prohibited. This applies to both `ImpairmentEngine` and `DeliveryEngine` (H-6 from SEC_REPORT.txt; HAZ-022; CWE-338).

---

## 5.3 Determinism and Testability

### REQ-5.3.1 — Reproducible Impairment Sequences
Given the same seed value and the same sequence of input messages, `ImpairmentEngine` produces an identical sequence of impairment decisions across all runs and platforms. This is guaranteed by:
- `PrngEngine` using a pure integer xorshift64 with no platform-specific state.
- All timing decisions being made against a monotonic clock injected via `IPosixSyscalls`, not wall time.
- No thread-level non-determinism within a single `ImpairmentEngine` instance.

### REQ-5.3.2 — Works with LocalSimHarness
`ImpairmentEngine` operates entirely on `MessageEnvelope` objects; it has no dependency on socket descriptors, network addresses, or OS I/O. The same `ImpairmentEngine` instance is used unchanged whether the transport backend is `TcpBackend` or `LocalSimHarness`. Test scenarios set a known seed, inject messages via `LocalSimHarness::inject()`, and verify the resulting impaired sequence deterministically.

---

## Configuration File Format

`impairment_config_load(path, &config)` reads a key=value text file. Recognized keys:

| Key | Type | Default | Description |
|---|---|---|---|
| `loss_enabled` | bool | false | Enable packet loss |
| `loss_probability` | float | 0.0 | Loss probability (0.0–1.0) |
| `jitter_enabled` | bool | false | Enable random jitter |
| `jitter_max_ms` | uint32 | 0 | Max jitter in milliseconds |
| `dup_enabled` | bool | false | Enable duplication |
| `dup_probability` | float | 0.0 | Duplication probability (0.0–1.0) |
| `dup_delay_ms` | uint32 | 0 | Additional delay for duplicates |
| `reorder_enabled` | bool | false | Enable reordering |
| `reorder_probability` | float | 0.0 | Reorder probability (0.0–1.0) |
| `reorder_delay_ms` | uint32 | 0 | Hold duration for reordered messages |
| `partition_enabled` | bool | false | Enable partition simulation |
| `partition_duration_ms` | uint32 | 0 | Duration of each partition interval |
| `partition_gap_ms` | uint32 | 0 | Gap between partition intervals |
| `fixed_latency_ms` | uint32 | 0 | Fixed one-way latency added to every message |
| `prng_seed` | uint64 | 0 | Explicit seed (0 = use entropy source) |

Unknown keys are ignored. Values out of valid range are clamped with a `WARNING_LO` log entry.
