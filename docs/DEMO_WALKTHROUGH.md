# Demo Walkthrough — messageEngine in 5 Minutes

This document walks through a live run of `build/server` and `build/client`, explaining every
log line so you can see exactly what the library does end-to-end.

The demo uses the plain TCP transport.  The TLS (`build/tls_demo`) and DTLS-UDP
(`build/dtls_demo`) demos follow the same pattern with an additional handshake phase.

> **Log format note:** Log lines below are shown in shortened form — timestamps, PID, and TID
> are omitted for readability. The actual format produced by the Logger is:
> ```
> [SSSSSSS.uuuuuu][PID][SEVERITY][TID][module][file.cpp:line] message
> ```
> For example: `[0000000.123456][1234][INFO    ][1234][Server][Server.cpp:237] Starting TCP server…`
> See [docs/LOGGING.md](LOGGING.md) for full format details.

---

## Prerequisites

```
make          # builds Server, Client, and the full test suite
```

---

## What the demo does

```
Terminal 1          Terminal 2
──────────────────  ──────────────────────────────────────
build/server        (wait ~1 s, then)  build/client

  ← client connects ────────────────────────────────┐
  ← HELLO frame ────────────────────────────────────┤  (identifies itself as node 2)
  ← DATA  "Hello from client #1" ──────────────────┤
  → ACK   ─────────────────────────────────────────┤
  → echo DATA "Hello from client #1" ──────────────┤
  ← ACK   ─────────────────────────────────────────┘
  ... repeated for messages #2 – #5 ...
^C                  (exits when all 5 echoes received)
```

**Key concepts visible in the output:**

| Term | What it means |
|---|---|
| `message_id` | 64-bit monotonic counter, unique per sender; used for dedup and ACK matching |
| `reliability=2` | `RELIABLE_RETRY` — send, track ACK, retry up to 3× if no ACK arrives |
| HELLO frame | First frame a TCP client sends; registers its `NodeId` with the server so unicast routing works |
| `pump_retries` | Called every loop iteration; resends any message whose retry timer has fired |
| `sweep_ack_timeouts` | Called every loop iteration; declares failure for any message whose ACK window expired |

---

## Terminal 1 — Server

```
$ build/server
```

### Startup

```
[INFO    ][Server][Server.cpp:237] Starting TCP server on port 9000
```
> `main()` parsed the port (default 9000) and is entering init.

```
[INFO    ][TcpBackend][TcpBackend.cpp:138] Server listening on 0.0.0.0:9000
```
> `TcpBackend::init()` bound the listen socket and called `listen()`.
> The server is now accepting connections on all interfaces.

```
[INFO    ][Server][Server.cpp:272] TcpBackend initialized
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:352] Initialized channel=0, local_id=1
[INFO    ][Server][Server.cpp:279] DeliveryEngine initialized
[INFO    ][Server][Server.cpp:291] Entering main loop. Press Ctrl+C to exit.
```
> `DeliveryEngine::init()` wired together the transport backend, ACK tracker,
> retry manager, and duplicate filter.  `local_id=1` is the server's `NodeId`.
> The server now polls for incoming connections in a bounded loop
> (`MAX_LOOP_ITERS = 100 000`).

---

### Client connects

```
[INFO    ][TcpBackend][TcpBackend.cpp:259] Accepted client 0, total clients: 1
```
> `accept()` returned a new file descriptor.  The server stores it in slot 0
> of its fixed-size connection table (`MAX_TCP_CONNECTIONS` slots).
> No `NodeId` is assigned yet — the slot is "unregistered" until a HELLO arrives.

```
[INFO    ][TcpBackend][TcpBackend.cpp:961] HELLO from client slot 0 node_id=2
```
> The client sent a HELLO frame (MessageType 4, zero-length payload) declaring
> `source_id=2`.  The server records `NodeId 2 → slot 0` in its routing table.
> Subsequent DATA frames from slot 0 will be validated against `node_id=2`;
> a mismatch is a source-spoofing attempt and is silently dropped (REQ-6.1.11).

---

### Message exchange (×5)

The following block repeats five times (once per client message).

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:1022] Received data message_id=1 from src=2, length=22
[INFO    ][Server][Server.cpp:200] Received msg#1 from node 2, len 22:
Hello from client #1
```
> `DeliveryEngine::receive()` dequeued an incoming DATA envelope, checked for
> duplicates (DuplicateFilter), verified it was addressed to `local_id=1`, and
> returned it to the application.  The server prints the payload as a string.

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:775] Sent message_id=1, reliability=2
```
> `send_echo_reply()` built a reply (source ↔ destination swapped, same payload),
> handed it to `DeliveryEngine::send()`, which serialized it, recorded it in the
> ACK tracker and retry manager, and passed it to `TcpBackend::send()`.
> `reliability=2` = `RELIABLE_RETRY`: the server will retry this echo up to 3×
> if the client does not ACK it.

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:161] Received ACK for message_id=1 from src=2
```
> The client sent an ACK for the echo.  `DeliveryEngine` cancelled the retry slot
> and the ACK-timeout entry for `message_id=1`.  The echo is now confirmed delivered.

---

### Shutdown

```
[INFO    ][Server][Server.cpp:298] Stop flag set; exiting loop
```
> `SIGINT` (Ctrl+C) set `g_stop_flag`; the loop checked it at the top of the
> next iteration and broke cleanly.

```
[INFO    ][TcpBackend][TcpBackend.cpp:776] Transport closed
[INFO    ][Server][Server.cpp:308] Server stopped. Messages received: 5, sent: 5
```
> All 5 DATA messages received and 5 echo replies sent.  Clean exit (code 0).

---

## Terminal 2 — Client

```
$ build/client
```

### Startup

```
[INFO    ][Client][Client.cpp:266] Starting TCP client connecting to 127.0.0.1:9000
[INFO    ][TcpBackend][TcpBackend.cpp:224] Connected to 127.0.0.1:9000
```
> `TcpBackend::init()` called `connect()` with a 5-second timeout.
> TCP 3-way handshake completed.

```
[INFO    ][Client][Client.cpp:303] TcpBackend initialized
[INFO    ][TcpBackend][TcpBackend.cpp:865] HELLO sent: local_id=2
```
> `DeliveryEngine::init()` called `register_local_id(2)`, which caused
> `TcpBackend` to send a HELLO frame on the wire before any DATA frame
> is transmitted (REQ-6.1.8).

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:352] Initialized channel=0, local_id=2
[INFO    ][Client][Client.cpp:310] DeliveryEngine initialized
[INFO    ][Client][Client.cpp:315] Sending 5 test messages...
```

---

### Message exchange (×5)

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:775] Sent message_id=1, reliability=2
[INFO    ][Client][Client.cpp:236] Sent message #1
```
> `send_test_message()` built a `MessageEnvelope` with payload
> `"Hello from client #1"`, 5-second expiry, and `RELIABLE_RETRY` semantics,
> then called `engine.send()`.

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:161] Received ACK for message_id=1 from src=1
```
> The server sent an ACK after receiving the DATA.  The ACK cleared the client's
> retry slot for `message_id=1` — no retransmission will occur for this message.

```
[INFO    ][DeliveryEngine][DeliveryEngine.cpp:1022] Received data message_id=1 from src=1, length=22
[INFO    ][Client][Client.cpp:161] Received echo reply: msg_id=1, len=22
```
> The echo arrived.  `DeliveryEngine::receive()` passed it up; `wait_for_echo()`
> matched it as a DATA envelope and returned success.

---

### Completion

```
[INFO    ][TcpBackend][TcpBackend.cpp:776] Transport closed
[INFO    ][Client][Client.cpp:344] Client completed. Sent: 5, Echo replies received: 5
```
> All 5 echoes received.  `exit_code = 0`.

---

## What to explore next

| Experiment | How to run it |
|---|---|
| Watch retry fire | Kill the server mid-run; the client will log `Retried message_id=N` up to 3× then `ACK timeout` |
| Add impairment (loss/latency) | Edit `TransportConfig` in `Client.cpp` to enable `ImpairmentConfig`; rebuild |
| TLS transport | `build/tls_demo server` and `build/tls_demo client` — same log structure, plus mbedTLS handshake lines |
| DTLS-UDP transport | `build/dtls_demo server` and `build/dtls_demo client` |
| Run the full test suite | `make run_tests` — 24 test binaries covering every component in isolation |
| Branch coverage report | `make coverage` — shows per-file SC function coverage |

For the full architecture and design rationale, see:
- [`docs/DESIGN_PATTERNS.md`](DESIGN_PATTERNS.md) — design patterns and why they were chosen
- [`docs/HAZARD_ANALYSIS.md`](HAZARD_ANALYSIS.md) — safety-critical function classification
- [`CLAUDE.md`](../CLAUDE.md) — all requirements (REQ-x.x IDs referenced in log comments)
