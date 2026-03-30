Use Case 28: DeliveryEngine Initializes All Sub-Components Before First Use
============================================================================

## 1. Use Case Overview

Actor:        System initialization routine (caller owns the DeliveryEngine object).
Trigger:      The system, after allocating or constructing a DeliveryEngine instance, calls
              DeliveryEngine::init() to bring the engine into a ready state.
Precondition: A valid TransportInterface* has already been constructed and initialized.
              A ChannelConfig struct has been populated with channel parameters.
              A valid NodeId has been chosen for this endpoint (must not be NODE_ID_INVALID).
Postcondition: All four owned sub-components (AckTracker, RetryManager, DuplicateFilter,
               MessageIdGen) are fully initialized. m_initialized == true. No messages
               have been sent or received. The engine is ready for send()/receive() calls.
Scope:        Initialization phase only. No dynamic allocation occurs after this function
              returns (Power of 10 rule 3).

This use case covers the complete initialization call chain from the perspective of one
DeliveryEngine instance serving one logical channel.


## 2. Entry Points

Primary entry point:
  DeliveryEngine::init(TransportInterface* transport,
                       const ChannelConfig& cfg,
                       NodeId local_id)
  File: src/core/DeliveryEngine.cpp, lines 15-49.

Transitively entered during init():
  AckTracker::init()        -- src/core/AckTracker.cpp, line 22.
  RetryManager::init()      -- src/core/RetryManager.cpp, line 16.
  DuplicateFilter::init()   -- src/core/DuplicateFilter.cpp, line 22.
  MessageIdGen::init(seed)  -- src/core/MessageId.hpp, line 33 (inline).


## 3. End-to-End Control Flow (Step-by-Step)

Step 1  -- Caller invokes DeliveryEngine::init(transport, cfg, local_id).
           [DeliveryEngine.cpp:15]

Step 2  -- Two precondition assertions fire:
             assert(transport != nullptr)         [line 19]
             assert(local_id != NODE_ID_INVALID)  [line 20]
           Both must pass; a failure aborts execution in debug builds.

Step 3  -- Member fields assigned from parameters:
             m_transport = transport              [line 22]
             m_cfg       = cfg    (copy)          [line 23]
             m_local_id  = local_id               [line 24]
             m_initialized = false                [line 25]
           m_initialized is explicitly set to false before sub-component init begins,
           ensuring any re-entrant or interrupted call cannot see a partially ready state.

Step 4  -- m_ack_tracker.init() is called.        [line 28]
           Inside AckTracker::init() [AckTracker.cpp:22]:
             a. memset(m_slots, 0, sizeof(m_slots)) -- zeroes the entire fixed slot array
                of ACK_TRACKER_CAPACITY entries.   [line 24]
             b. m_count = 0U.                       [line 26]
             c. Post-condition assertion: assert(m_count == 0U). [line 29]
             d. Verification loop (bounded by ACK_TRACKER_CAPACITY):
                  for i in [0, ACK_TRACKER_CAPACITY):
                    assert(m_slots[i].state == EntryState::FREE)  [lines 33-35]
                Each slot was zeroed by memset, so state==FREE (assumes FREE==0).
           AckTracker::init() returns (void).

Step 5  -- m_retry_manager.init() is called.      [line 29]
           Inside RetryManager::init() [RetryManager.cpp:16]:
             a. assert(true) -- unconditional entry marker.       [line 19]
             b. m_count = 0U.                                      [line 21]
             c. m_initialized = true.                              [line 22]
             d. Initialization loop (bounded by ACK_TRACKER_CAPACITY):
                  for i in [0, ACK_TRACKER_CAPACITY):
                    assert(i < ACK_TRACKER_CAPACITY)              [line 26]
                    m_slots[i].active       = false               [line 27]
                    m_slots[i].retry_count  = 0U                  [line 28]
                    m_slots[i].backoff_ms   = 0U                  [line 29]
                    m_slots[i].next_retry_us = 0ULL               [line 30]
                    m_slots[i].expiry_us    = 0ULL                [line 31]
                    m_slots[i].max_retries  = 0U                  [line 32]
                    envelope_init(m_slots[i].env)                 [line 33]
             e. Post-condition assertions:
                    assert(m_count == 0U)                         [line 37]
                    assert(m_initialized)                         [line 38]
           RetryManager::init() returns (void).

Step 6  -- m_dedup.init() is called.              [line 30]
           Inside DuplicateFilter::init() [DuplicateFilter.cpp:22]:
             a. memset(m_window, 0, sizeof(m_window)) -- zeroes the dedup window of
                DEDUP_WINDOW_SIZE entries.          [line 24]
             b. m_next  = 0U.                       [line 26]
             c. m_count = 0U.                       [line 27]
             d. Post-condition assertions:
                    assert(m_next == 0U)            [line 30]
                    assert(m_count == 0U)           [line 31]
           DuplicateFilter::init() returns (void).

Step 7  -- Seed computation for MessageIdGen.      [lines 34-37]
             uint64_t id_seed = static_cast<uint64_t>(local_id)
             if (id_seed == 0ULL): id_seed = 1ULL   [ensures non-zero seed]
           This guarantees a deterministic, node-specific starting sequence.

Step 8  -- m_id_gen.init(id_seed) is called.      [line 38]
           Inside MessageIdGen::init(seed) [MessageId.hpp:33]:
             a. assert(seed != 0ULL) -- confirms seed invariant.  [line 36]
             b. m_next = seed.                                     [line 38]
             c. Post-condition assertion: assert(m_next == seed).  [line 41]
           MessageIdGen::init() returns (void).

Step 9  -- m_initialized = true.                   [line 40]

Step 10 -- Two post-condition assertions fire:
             assert(m_initialized)                 [line 43]
             assert(m_transport != nullptr)        [line 44]

Step 11 -- Logger::log(INFO, "DeliveryEngine", "Initialized channel=%u, local_id=%u",
           cfg.channel_id, local_id) is called.   [lines 46-48]

Step 12 -- DeliveryEngine::init() returns (void). Control returns to caller.


## 4. Call Tree (Hierarchical)

DeliveryEngine::init(transport, cfg, local_id)       [DeliveryEngine.cpp:15]
  |
  +-- [assertions] transport != nullptr, local_id != NODE_ID_INVALID
  |
  +-- m_ack_tracker.init()                           [AckTracker.cpp:22]
  |     +-- memset(m_slots, 0, sizeof(m_slots))
  |     +-- m_count = 0U
  |     +-- [assertion] m_count == 0U
  |     +-- [loop: 0..ACK_TRACKER_CAPACITY)
  |           +-- [assertion] m_slots[i].state == FREE
  |
  +-- m_retry_manager.init()                         [RetryManager.cpp:16]
  |     +-- m_count = 0U
  |     +-- m_initialized = true
  |     +-- [loop: 0..ACK_TRACKER_CAPACITY)
  |     |     +-- [assertion] i < ACK_TRACKER_CAPACITY
  |     |     +-- m_slots[i].active = false
  |     |     +-- m_slots[i].retry_count = 0U
  |     |     +-- m_slots[i].backoff_ms = 0U
  |     |     +-- m_slots[i].next_retry_us = 0ULL
  |     |     +-- m_slots[i].expiry_us = 0ULL
  |     |     +-- m_slots[i].max_retries = 0U
  |     |     +-- envelope_init(m_slots[i].env)
  |     +-- [assertions] m_count == 0U, m_initialized
  |
  +-- m_dedup.init()                                 [DuplicateFilter.cpp:22]
  |     +-- memset(m_window, 0, sizeof(m_window))
  |     +-- m_next = 0U
  |     +-- m_count = 0U
  |     +-- [assertions] m_next == 0U, m_count == 0U
  |
  +-- [seed computation] id_seed = (uint64_t)local_id; if 0 -> 1
  |
  +-- m_id_gen.init(id_seed)                         [MessageId.hpp:33]
  |     +-- [assertion] seed != 0ULL
  |     +-- m_next = seed
  |     +-- [assertion] m_next == seed
  |
  +-- m_initialized = true
  +-- [assertions] m_initialized, m_transport != nullptr
  +-- Logger::log(INFO, ...)


## 5. Key Components Involved

Component            Type              File                         Role
-----------          --------          ----                         ----
DeliveryEngine       class             src/core/DeliveryEngine.cpp  Coordinator; owns all below.
AckTracker           class (member)    src/core/AckTracker.cpp      Tracks PENDING ACKs in fixed slots.
RetryManager         class (member)    src/core/RetryManager.cpp    Schedules messages for retry.
DuplicateFilter      class (member)    src/core/DuplicateFilter.cpp Sliding-window dedup suppression.
MessageIdGen         class (member)    src/core/MessageId.hpp       Counter-based unique ID generator.
TransportInterface*  pointer (member)  src/core/TransportInterface.hpp  Abstraction for send/receive.
ChannelConfig        struct (member)   src/core/ChannelConfig.hpp   Channel params (timeouts, retries).
Logger               static util       src/core/Logger.hpp          INFO log on successful init.


## 6. Branching Logic / Decision Points

Branch 1: assert(transport != nullptr) [line 19]
  - If transport IS nullptr: assert fires in debug builds; undefined behavior in release.
  - If transport is valid: execution continues.

Branch 2: assert(local_id != NODE_ID_INVALID) [line 20]
  - If local_id IS invalid: assert fires.
  - If local_id is valid: execution continues.

Branch 3: Seed coercion [lines 35-37]
  - if (id_seed == 0ULL): id_seed = 1ULL
  - This handles the edge case where local_id == 0 (or NODE_ID_INVALID was 0).
  - Guarantees the MessageIdGen seed is always non-zero.

Branch 4: AckTracker per-slot state assertion [lines 33-35]
  - If EntryState::FREE is NOT equal to 0 (the zero-initialized bit pattern), the
    loop assertion fires. The code relies on FREE==0 for the memset optimization.
    [ASSUMPTION: EntryState::FREE == 0 in the enum definition.]

Branch 5: RetryManager slot loop [lines 25-34]
  - Unlike AckTracker, RetryManager does NOT use memset; it initializes each field
    individually and calls envelope_init() per slot. This is safer against padding-
    related memset traps but is O(ACK_TRACKER_CAPACITY).

Branch 6: DuplicateFilter memset path [lines 24-27]
  - Full zero-fill followed by explicit counter reset. Relies on valid==false being 0.
    [ASSUMPTION: the valid bool field in the dedup window entry equals false when zeroed.]


## 7. Concurrency / Threading Behavior

DeliveryEngine::init() is intended to be called ONCE during system initialization,
before any concurrent access begins. The code contains no locks, mutexes, or atomics
in the init path.

AckTracker, RetryManager, and DuplicateFilter all use plain (non-atomic) fields.
None of these sub-components are thread-safe for concurrent calls.

RingBuffer (used by LocalSimHarness, not directly by DeliveryEngine) uses
std::atomic<uint32_t> with acquire/release for SPSC thread safety, but RingBuffer
is not touched during DeliveryEngine::init().

Conclusion: init() must complete before any thread calls send() or receive().
No synchronization is provided or needed within init() itself.


## 8. Memory and Ownership Semantics (C/C++ Specific)

Allocation model: ALL storage is statically allocated inside the DeliveryEngine object.
  - m_ack_tracker  : AckTracker value member  -- m_slots[ACK_TRACKER_CAPACITY] on stack/static.
  - m_retry_manager: RetryManager value member -- m_slots[ACK_TRACKER_CAPACITY] on stack/static.
  - m_dedup        : DuplicateFilter value member -- m_window[DEDUP_WINDOW_SIZE] on stack/static.
  - m_id_gen       : MessageIdGen value member  -- single uint64_t m_next field.

No dynamic (heap) allocation occurs anywhere in the init path (Power of 10 rule 3).

Pointer ownership:
  - m_transport is a NON-OWNING raw pointer. DeliveryEngine does not delete it.
    Lifetime management is the caller's responsibility.

ChannelConfig copy:
  - cfg is passed by const reference and copied into m_cfg at line 23. The caller's
    copy is independent after init() returns.

memset usage:
  - AckTracker::init() uses memset on m_slots (a plain-old-data array). Correct as
    long as EntryState::FREE == 0 and all slot fields have trivial zero representations.
  - DuplicateFilter::init() uses memset on m_window similarly (valid==false when 0).
  - RetryManager::init() avoids memset and initializes each field explicitly, which
    is safer but carries O(N) CPU cost.

envelope_init():
  - Called per slot in RetryManager. [ASSUMPTION: envelope_init() zero-initializes
    all MessageEnvelope fields without dynamic allocation.]


## 9. Error Handling Flow

DeliveryEngine::init() does NOT return a Result; it is declared void.
All error detection is via assert() (Power of 10 rule 5).

Assertion failures:
  - transport == nullptr        -> assert fires at line 19. Fatal in debug.
  - local_id == NODE_ID_INVALID -> assert fires at line 20. Fatal in debug.
  - AckTracker post-loop assert -> assert fires if memset left non-FREE state.
  - MessageIdGen seed==0        -> assert fires at MessageId.hpp:36 (caught by seed coercion
                                   at DeliveryEngine.cpp:35-37 before reaching init()).

Sub-component inits are all void; they carry no return-value error paths.

No error recovery is possible during init. If an assertion fails the system must be
restarted. This is consistent with the F-Prime FATAL error model for init-phase failures.

Logger::log() at the end is informational only; a failure there would not roll back init.


## 10. External Interactions

TransportInterface*:
  - The pointer is stored but the transport is NOT called during init().
  - No network I/O occurs.

Logger::log():
  - Called once at the end of DeliveryEngine::init() with Severity::INFO.
  - Outputs: channel_id and local_id values.
  - This is the only external call during init.

No file I/O, no OS calls, no socket interactions during init().


## 11. State Changes / Side Effects

Before init():
  m_transport     undefined (uninitialized object)
  m_cfg           undefined
  m_local_id      undefined
  m_initialized   undefined
  m_ack_tracker   uninitialized
  m_retry_manager uninitialized
  m_dedup         uninitialized
  m_id_gen        uninitialized

After init() completes:
  m_transport     = transport (caller-provided pointer)
  m_cfg           = copy of cfg parameter
  m_local_id      = local_id parameter
  m_initialized   = true

  m_ack_tracker:
    m_slots[0..ACK_TRACKER_CAPACITY-1].state = FREE (zeroed)
    m_count = 0

  m_retry_manager:
    m_slots[0..ACK_TRACKER_CAPACITY-1].active       = false
    m_slots[0..ACK_TRACKER_CAPACITY-1].retry_count  = 0
    m_slots[0..ACK_TRACKER_CAPACITY-1].backoff_ms   = 0
    m_slots[0..ACK_TRACKER_CAPACITY-1].next_retry_us = 0
    m_slots[0..ACK_TRACKER_CAPACITY-1].expiry_us    = 0
    m_slots[0..ACK_TRACKER_CAPACITY-1].max_retries  = 0
    m_slots[0..ACK_TRACKER_CAPACITY-1].env          = envelope_init() state
    m_count = 0
    m_initialized = true

  m_dedup:
    m_window[0..DEDUP_WINDOW_SIZE-1].valid = false (zeroed)
    m_window[0..DEDUP_WINDOW_SIZE-1].src   = 0
    m_window[0..DEDUP_WINDOW_SIZE-1].msg_id = 0
    m_next = 0
    m_count = 0

  m_id_gen:
    m_next = id_seed   (== (uint64_t)local_id, minimum 1)

Side effect: Logger::log() emits one INFO message.


## 12. Sequence Diagram (ASCII)

  Caller                 DeliveryEngine         AckTracker       RetryManager     DuplicateFilter    MessageIdGen
    |                         |                      |                 |                  |                |
    |-- init(t, cfg, id) ---> |                      |                 |                  |                |
    |                         |-- assert(t!=null)    |                 |                  |                |
    |                         |-- assert(id valid)   |                 |                  |                |
    |                         |-- m_transport=t      |                 |                  |                |
    |                         |-- m_cfg=cfg          |                 |                  |                |
    |                         |-- m_local_id=id      |                 |                  |                |
    |                         |-- m_initialized=false|                 |                  |                |
    |                         |                      |                 |                  |                |
    |                         |-- init() ----------> |                 |                  |                |
    |                         |                      |-- memset slots  |                  |                |
    |                         |                      |-- m_count=0     |                  |                |
    |                         |                      |-- assert loop   |                  |                |
    |                         |                      |                 |                  |                |
    |                         |-- init() -----------------------> |                  |                |
    |                         |                                   |-- m_count=0      |                |
    |                         |                                   |-- m_init=true    |                |
    |                         |                                   |-- slot loop      |                |
    |                         |                                   |                  |                |
    |                         |-- init() ---------------------------------> |                |
    |                         |                                             |-- memset win   |
    |                         |                                             |-- m_next=0     |
    |                         |                                             |-- m_count=0    |
    |                         |                                             |                |
    |                         |-- [seed = (uint64_t)local_id; if 0 -> 1]   |                |
    |                         |                                             |                |
    |                         |-- init(seed) -----------------------------------------> |
    |                         |                                                          |-- m_next=seed
    |                         |                                                          |-- assert
    |                         |                                                          |
    |                         |-- m_initialized = true                                   |
    |                         |-- assert(m_initialized)                                  |
    |                         |-- assert(m_transport!=null)                              |
    |                         |-- Logger::log(INFO, ...)                                 |
    |                         |                                                          |
    | <-- (void) ------------ |                                                          |


## 13. Initialization vs Runtime Flow

Initialization (this use case):
  - DeliveryEngine::init() is the sole init-phase entry point.
  - All four sub-components are initialized in order: AckTracker, RetryManager,
    DuplicateFilter, MessageIdGen.
  - No messages are sent or received.
  - All allocation occurs implicitly through object construction (value members).
  - After init(), m_initialized == true.

Runtime (post-init, NOT covered here):
  - send() uses m_id_gen.next() to assign message IDs.
  - send() uses m_ack_tracker.track() and m_retry_manager.schedule() for reliable classes.
  - receive() uses m_dedup.check_and_record() for RELIABLE_RETRY dedup.
  - pump_retries() drives m_retry_manager.collect_due() + send_via_transport().
  - sweep_ack_timeouts() drives m_ack_tracker.sweep_expired().

Any runtime call that reaches a sub-component without a prior init() would fail the
assert(m_initialized) guard in send()/receive() (lines 77, 151 of DeliveryEngine.cpp).
RetryManager also carries its own assert(m_initialized) check in every method.


## 14. Known Risks / Observations

Risk 1: No return value from init()
  The function is declared void. If an assertion fails in a release build (where
  assert() is compiled out), partial initialization state can result. The engine
  would appear to have m_initialized==true even if sub-components failed silently.
  Mitigation: assert() must remain enabled, or a Result return code should be added.

Risk 2: memset reliance on zero-equals-FREE assumption
  AckTracker::init() and DuplicateFilter::init() use memset to zero memory and then
  assert the resulting state matches FREE/false. If EntryState::FREE != 0 or the
  valid bool field has a non-zero false representation on a given platform, this is
  undefined behavior. The verification loop in AckTracker guards against this in
  debug builds.

Risk 3: Ordering of sub-component inits is implicit
  The code initializes AckTracker, RetryManager, DuplicateFilter, then MessageIdGen
  in that order (lines 28-38). There is no formal dependency between them; any order
  would be correct. However, if a future developer adds a cross-dependency, this
  implicit ordering could become a bug.

Risk 4: m_initialized set to false at entry
  If init() is called a second time on an already-initialized engine (e.g., after a
  FATAL event), m_initialized is set to false at line 25, making the engine temporarily
  non-functional during re-init. If any other thread observes m_initialized in this
  window, it will see false. No re-init guard exists.

Risk 5: ACK_TRACKER_CAPACITY used for both AckTracker and RetryManager loop bounds
  Both sub-components share the same capacity constant. This is intentional but means
  the RetryManager slot table is identically sized to the AckTracker. If the retry
  load significantly differs from the ACK tracking load, one table will be wasteful.

Observation: envelope_init() is called ACK_TRACKER_CAPACITY times in RetryManager::init().
  This is O(N) initialization cost. For large capacity values this may add measurable
  latency to the system startup path.


## 15. Unknowns / Assumptions

[ASSUMPTION] EntryState::FREE == 0.
  Required for AckTracker::init()'s memset optimization to be correct. The enum
  definition was not provided in the read files; this is inferred from the verification
  loop assertion that follows memset.

[ASSUMPTION] DuplicateFilter window entry's `valid` bool field equals false when
  the byte value is 0x00. Standard C/C++ guarantees this for bool, so this assumption
  is safe on all conforming platforms.

[ASSUMPTION] envelope_init() zero-initializes a MessageEnvelope without any dynamic
  allocation. The function is referenced in RetryManager::init() but its definition
  was not in the read files.

[ASSUMPTION] NODE_ID_INVALID evaluates to some specific sentinel value (likely 0 or
  0xFFFF). The assertion at line 20 guards against it but the definition is in
  Types.hpp which was not read.

[ASSUMPTION] ACK_TRACKER_CAPACITY and DEDUP_WINDOW_SIZE are compile-time constants
  defined in their respective header files (not read). Their values determine the
  actual memory footprint and loop iteration counts.

[ASSUMPTION] Logger::log() is non-blocking, does not allocate, and always returns
  successfully. No return value is checked (Logger is treated as fire-and-forget).

[UNKNOWN] Whether re-initialization (calling init() more than once on the same object)
  is an intended use pattern. No guard prevents it, but no documentation supports it.