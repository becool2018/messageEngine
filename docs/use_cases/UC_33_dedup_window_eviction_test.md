## 1. Use Case Overview

Use Case ID : UC_33
Title       : Dedup Window Eviction Test (Sliding-Window Wraparound)
Source File : tests/test_DuplicateFilter.cpp, function test_window_wraparound() (lines 114-154)
Scope       : Verifies that the DuplicateFilter's ring-buffer correctly evicts the oldest
              entries when the window overflows, and that evicted entries are no longer
              reported as duplicates.

Description:
  DEDUP_WINDOW_SIZE = 128 (Types.hpp:21).
  The test:
    Phase A: Records 128 unique (source=1, msg_id=0..127) entries to fill the window.
    Phase B: Asserts all 128 are seen as duplicates.
    Phase C: Records 5 more entries (msg_id=128..132), each of which overwrites the
             oldest slot via the round-robin m_next pointer (wrapping from 127 -> 0).
    Phase D: Asserts that msg_ids 0..4 are no longer duplicates (evicted).
    Phase E: Asserts that msg_ids 5..132 are still duplicates.


## 2. Entry Points

Primary entry point (process-wide):
  main()  [test_DuplicateFilter.cpp:159]
    -> calls test_window_wraparound()  [line 191]

Secondary entry point (the test function itself):
  test_window_wraparound()  [test_DuplicateFilter.cpp:114]

Objects constructed on the stack:
  - DuplicateFilter filter   [line 115]


## 3. End-to-End Control Flow (Step-by-Step)

--- SETUP PHASE ---

Step 1  main() calls test_window_wraparound() [line 191]

Step 2  Stack allocation: DuplicateFilter filter [line 115]
  [ASSUMPTION] No user-provided constructor; fields are indeterminate until init().

Step 3  filter.init() [line 116]
  -- Enters DuplicateFilter::init() [DuplicateFilter.cpp:21]

  Step 3a  memset(m_window, 0, sizeof(m_window)) [line 24]
    sizeof(m_window) = sizeof(Entry) * 128
    Each Entry: { NodeId src (4B), uint64_t msg_id (8B), bool valid (1B) + padding }
    [ASSUMPTION] sizeof(Entry) is at least 13 bytes; with alignment, likely 16 bytes.
    All 128 Entry slots: src=0, msg_id=0, valid=false.

  Step 3b  m_next = 0U [line 26]
    Write pointer starts at slot 0.

  Step 3c  m_count = 0U [line 27]

  Step 3d  Postcondition assertions [lines 30-31]:
    assert(m_next == 0U)   -- passes
    assert(m_count == 0U)  -- passes

--- PHASE A: Fill window with 128 entries ---

Step 4  First loop [lines 121-124]:
  for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i)   // 0..127
    msg_id = static_cast<uint64_t>(i)
    filter.record(1U, msg_id)

  Each call to record() [DuplicateFilter.cpp:61]:

  Iteration i=0:  record(1U, 0ULL)
    Precondition assertions [lines 64-65]:
      assert(m_next < 128)   -> 0 < 128  passes
      assert(m_count <= 128) -> 0 <= 128 passes

    m_window[0].src    = 1U
    m_window[0].msg_id = 0ULL
    m_window[0].valid  = true

    m_next = (0 + 1) % 128 = 1
    m_count < 128  ->  m_count = 1

    Postcondition assertions:
      assert(1 < 128)   passes
      assert(1 <= 128)  passes

  Iteration i=1:  record(1U, 1ULL)
    m_window[1] filled; m_next=2; m_count=2.

  ...

  Iteration i=127: record(1U, 127ULL)
    Precondition assertions:
      m_next = 127 < 128  passes
      m_count = 127 <= 128  passes

    m_window[127].src    = 1U
    m_window[127].msg_id = 127ULL
    m_window[127].valid  = true

    m_next = (127 + 1) % 128 = 0      <-- WRAP: m_next returns to slot 0
    m_count = 127 < 128  ->  m_count = 128  <-- count reaches maximum

    Postcondition:
      assert(0 < 128)    passes
      assert(128 <= 128) passes

  State after Phase A:
    m_window[0..127]: all valid=true, src=1, msg_id matches index i
    m_next  = 0   (has wrapped around to beginning)
    m_count = 128 (window is exactly full)

--- PHASE B: Verify all 128 are duplicates ---

Step 5  Second loop [lines 127-130]:
  for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i)
    assert(filter.is_duplicate(1U, static_cast<uint64_t>(i)) == true)

  Each is_duplicate(1U, i) call [DuplicateFilter.cpp:38]:
    Linear scan of all 128 slots:
      for (uint32_t j = 0; j < 128; ++j)
        assert(j < 128)  -- Power of 10 loop invariant
        if (m_window[j].valid && m_window[j].src==1 && m_window[j].msg_id==i)
          return true
    Match is found at slot j==i in every case (1:1 placement from Phase A).

    Postcondition:
      assert(m_count <= 128) -- 128 <= 128, passes
    returns true.

  All 128 assertions pass.

--- PHASE C: Insert 5 more entries (trigger eviction of oldest 5) ---

Step 6  Third loop [lines 133-136]:
  for (uint32_t i = 0U; i < 5U; ++i)
    msg_id = static_cast<uint64_t>(128 + i)   // 128, 129, 130, 131, 132
    filter.record(1U, msg_id)

  Recall state entering Phase C:
    m_next  = 0   (pointing at slot 0, which holds {src=1, msg_id=0, valid=true})
    m_count = 128

  Iteration i=0: record(1U, 128ULL)
    Precondition:
      assert(m_next=0 < 128)    passes
      assert(m_count=128 <= 128) passes

    m_window[0].src    = 1U    (OVERWRITES slot 0; was {1, 0, true})
    m_window[0].msg_id = 128ULL
    m_window[0].valid  = true

    m_next = (0 + 1) % 128 = 1
    m_count == 128 (== DEDUP_WINDOW_SIZE)  ->  NOT incremented (already at max)

    Postcondition:
      assert(1 < 128)    passes
      assert(128 <= 128) passes

    EVICTION: The entry (src=1, msg_id=0) that was in slot 0 is permanently gone.
    The new entry (src=1, msg_id=128) now occupies slot 0.

  Iteration i=1: record(1U, 129ULL)
    m_window[1] overwritten: {1, 1, true} -> {1, 129, true}
    m_next = 2; m_count stays 128.
    EVICTION: (src=1, msg_id=1) gone from slot 1.

  Iteration i=2: record(1U, 130ULL)
    m_window[2] overwritten: {1, 2, true} -> {1, 130, true}
    m_next = 3; m_count stays 128.
    EVICTION: (src=1, msg_id=2) gone from slot 2.

  Iteration i=3: record(1U, 131ULL)
    m_window[3] overwritten: {1, 3, true} -> {1, 131, true}
    m_next = 4; m_count stays 128.
    EVICTION: (src=1, msg_id=3) gone from slot 3.

  Iteration i=4: record(1U, 132ULL)
    m_window[4] overwritten: {1, 4, true} -> {1, 132, true}
    m_next = 5; m_count stays 128.
    EVICTION: (src=1, msg_id=4) gone from slot 4.

  State after Phase C:
    Slot 0:  {src=1, msg_id=128, valid=true}  (was 0)
    Slot 1:  {src=1, msg_id=129, valid=true}  (was 1)
    Slot 2:  {src=1, msg_id=130, valid=true}  (was 2)
    Slot 3:  {src=1, msg_id=131, valid=true}  (was 3)
    Slot 4:  {src=1, msg_id=132, valid=true}  (was 4)
    Slots 5..127: unchanged {src=1, msg_id=5..127, valid=true}
    m_next  = 5
    m_count = 128

--- PHASE D: Verify evicted entries (msg_id 0-4) are no longer duplicates ---

Step 7  Fourth loop [lines 140-144]:
  for (uint32_t i = 0U; i < 5U; ++i)
    is_dup = filter.is_duplicate(1U, static_cast<uint64_t>(i))
    assert(is_dup == false)

  is_duplicate(1U, 0ULL):
    Linear scan of all 128 slots:
      Slot 0: valid=true, src=1, msg_id=128 != 0  -> no match
      Slot 1: msg_id=129 != 0  -> no match
      Slot 2: msg_id=130 != 0  -> no match
      Slot 3: msg_id=131 != 0  -> no match
      Slot 4: msg_id=132 != 0  -> no match
      Slots 5..127: msg_id=5..127 != 0  -> no match
    Loop exhausted.  assert(m_count<=128) passes.  returns false.

  assert(false == false)  passes.

  Same outcome for msg_ids 1, 2, 3, 4 (each was overwritten by 129, 130, 131, 132).

--- PHASE E: Verify surviving entries (msg_id 5-132) are still duplicates ---

Step 8  Fifth loop [lines 147-151]:
  for (uint32_t i = 5U; i < DEDUP_WINDOW_SIZE + 5U; ++i)   // i = 5..132
    is_dup = filter.is_duplicate(1U, static_cast<uint64_t>(i))
    assert(is_dup == true)

  For i=5 (msg_id=5):
    Slot 5: msg_id=5, src=1, valid=true -> match -> returns true.

  For i=6..127 (msg_ids 6..127):
    Each found in slots 6..127 respectively (unchanged from Phase A).

  For i=128..132 (msg_ids 128..132):
    Each found in slots 0..4 respectively (written in Phase C).

  All 128 assertions (5 from new + 123 from old) pass.

Step 9  return true [line 153]

Step 10  main() prints "PASS: test_window_wraparound\n"


## 4. Call Tree (Hierarchical)

main()  [test_DuplicateFilter.cpp:159]
  test_window_wraparound()  [line 114]
    filter.init()  [DuplicateFilter.cpp:21]
      memset(m_window, ...)                                -- CRT
    [Phase A loop x128]
      filter.record(1U, msg_id)  [DuplicateFilter.cpp:61]
        (write slot, advance m_next, update m_count)
    [Phase B loop x128]
      filter.is_duplicate(1U, msg_id)  [DuplicateFilter.cpp:38]
        (linear scan 128 slots)
    [Phase C loop x5]
      filter.record(1U, msg_id)  [DuplicateFilter.cpp:61]
        (overwrite oldest slot, m_next advances, m_count unchanged)
    [Phase D loop x5]
      filter.is_duplicate(1U, msg_id)  [DuplicateFilter.cpp:38]
        (linear scan 128 slots; no match; returns false)
    [Phase E loop x128]
      filter.is_duplicate(1U, msg_id)  [DuplicateFilter.cpp:38]
        (linear scan 128 slots; match found; returns true)


## 5. Key Components Involved

Component           | File                             | Role
--------------------|----------------------------------|--------------------------------
test_window_wraparound | test_DuplicateFilter.cpp:114  | Test driver
DuplicateFilter     | src/core/DuplicateFilter.hpp     | Ring-buffer dedup class
                    | src/core/DuplicateFilter.cpp     | init/record/is_duplicate impl
DuplicateFilter::Entry | (inner struct in hpp:49)      | POD: {NodeId src, uint64_t
                    |                                  | msg_id, bool valid}
Types.hpp           | src/core/Types.hpp               | DEDUP_WINDOW_SIZE=128,
                    |                                  | NodeId=uint32_t, Result enum


## 6. Branching Logic / Decision Points

Decision 1 -- record() line 78: if (m_count < DEDUP_WINDOW_SIZE)
  Phase A (i=0..127): m_count < 128 for all iterations -> m_count incremented.
  Phase C (i=128..132): m_count == 128 for all iterations -> NOT incremented.
  This is the eviction gate: no `valid = false` is ever set on the old slot; the new
  data is simply written over it.  The old (src, msg_id) pair is silently replaced.

Decision 2 -- is_duplicate() line 46: if (valid && src==src && msg_id==msg_id)
  Phase B: match always found at slot j==i; returns true.
  Phase D: no slot contains msg_ids 0..4; scan exhausts all 128; returns false.
  Phase E: match found for each queried msg_id; returns true.

Decision 3 -- m_next modulo wrap: (m_next + 1U) % DEDUP_WINDOW_SIZE
  After filling 128 entries, m_next wrapped from 127 to 0.
  Phase C writes start at slot 0, advancing to 5 after the 5 insertions.
  This is the round-robin (FIFO-style) eviction policy.

Decision 4 -- main() line 191: if (!test_window_wraparound())
  Returns true -> else branch -> PASS printed.


## 7. Concurrency / Threading Behavior

Single-threaded. DuplicateFilter has no mutexes or atomic members.

m_next (uint32_t) and m_count (uint32_t) are modified only by record().
m_window[] entries are modified only by record() and read only by is_duplicate().
There is no concurrent access; all operations are sequentially ordered in
test_window_wraparound().

In a production multi-threaded context, DuplicateFilter would require external
synchronisation before calling record() and is_duplicate() from multiple threads.
[OBSERVATION] The class does not document or enforce thread safety.


## 8. Memory & Ownership Semantics (C/C++ Specific)

DuplicateFilter has a single member:
  Entry m_window[128]  -- fixed array, no dynamic allocation.
  uint32_t m_next      -- 4 bytes
  uint32_t m_count     -- 4 bytes

Sizes:
  Entry: {NodeId(4) + uint64_t(8) + bool(1) + likely 3-7 bytes padding}
  With 8-byte alignment: sizeof(Entry) = 16 bytes [ASSUMPTION based on typical ABI].
  m_window total: 16 * 128 = 2048 bytes.
  Total DuplicateFilter on stack: ~2060 bytes.  Well within typical stack limits.

memset in init() zeroes the entire m_window array:
  sizeof(m_window) = sizeof(Entry) * 128.
  After memset: all valid=false, src=0, msg_id=0.
  This is correct for POD structs (no vtables, no non-trivial constructors).

record() writes directly into m_window[m_next]:
  No allocation; pure field assignment.
  Overwrites whatever was previously at that slot (the "eviction").
  The old (src, msg_id) is not saved; it is simply gone.

is_duplicate() reads m_window[0..127] sequentially; no writes.
  Declared const; correct.

Ownership: DuplicateFilter is on the stack of test_window_wraparound().
  Lifetime: from its definition [line 115] to the function return [line 153].
  No pointers to m_window or its entries escape the object.


## 9. Error Handling Flow

No error codes are returned from record() (void) or is_duplicate() (bool).
check_and_record() returns Result::ERR_DUPLICATE or Result::OK, but is NOT called
in test_window_wraparound(); only record() and is_duplicate() are used directly.

Failure modes in this test:
  - assert() in record() fires if m_next >= 128 (impossible: modulo always < 128)
    or m_count > 128 (impossible: capped at 128).
  - assert() in is_duplicate() fires if loop index goes out of bounds (impossible:
    bounded by DEDUP_WINDOW_SIZE).
  - Test assertion assert(is_dup == false) in Phase D fires if an evicted entry is
    still somehow found.  That would indicate a bug in the round-robin eviction.
  - Test assertion assert(is_dup == true) in Phase E fires if a surviving entry is
    not found.  That would indicate data corruption in m_window.

All assert() failures call abort() with no cleanup.  No exceptions, no Result codes.


## 10. External Interactions

None.  DuplicateFilter has no I/O, no socket calls, no Logger calls.

CRT interactions:
  memset()  -- called once during init().
  assert()  -- called 2+ times per method (Power of 10 rule 5).

printf() -- called in main() after the test returns, not inside the test function.

No network, file system, or OS calls anywhere on this path.


## 11. State Changes / Side Effects

State of filter after init():
  m_window[0..127]: all {src=0, msg_id=0, valid=false}
  m_next  = 0
  m_count = 0

State after Phase A (128 records):
  m_window[i] = {src=1, msg_id=i, valid=true}  for i in [0,127]
  m_next  = 0  (wrapped around)
  m_count = 128

State after Phase C (5 more records):
  m_window[0]  = {src=1, msg_id=128, valid=true}  (was msg_id=0)
  m_window[1]  = {src=1, msg_id=129, valid=true}  (was msg_id=1)
  m_window[2]  = {src=1, msg_id=130, valid=true}  (was msg_id=2)
  m_window[3]  = {src=1, msg_id=131, valid=true}  (was msg_id=3)
  m_window[4]  = {src=1, msg_id=132, valid=true}  (was msg_id=4)
  m_window[5..127]: unchanged {src=1, msg_id=5..127, valid=true}
  m_next  = 5
  m_count = 128  (unchanged; was already at cap)

is_duplicate() and the test assertions produce no state changes (read-only for filter).

Side effects visible outside the test:
  stdout: "PASS: test_window_wraparound\n" (from main()).
  No other side effects.


## 12. Sequence Diagram (ASCII)

  main()   test_window_wraparound()   DuplicateFilter  Entry[128]
    |               |                       |               |
    |--test_wrap()-->                        |               |
    |               |                       |               |
    |         filter.init() --------------->|               |
    |               |                       |--memset(0)--->| all valid=false
    |               |                       | m_next=0      |
    |               |                       | m_count=0     |
    |               |<----------------------|               |
    |               |                       |               |
    |   [loop A: i=0..127]                   |               |
    |               |                       |               |
    |         filter.record(1,i) ---------->|               |
    |               |                       |--write[i]---->| valid=T,src=1,id=i
    |               |                       | m_next=(i+1)%128              |
    |               |                       | m_count++ (capped at 128)     |
    |               |<----------------------|               |
    |   [end loop A: m_next=0, m_count=128] |               |
    |               |                       |               |
    |   [loop B: i=0..127]                   |               |
    |               |                       |               |
    |         filter.is_duplicate(1,i) ---->|               |
    |               |                       |--scan[0..127]->| find slot i
    |               |                       |<--match(true)--|
    |               |<--true----------------|               |
    |         assert(true) pass             |               |
    |   [end loop B]                         |               |
    |               |                       |               |
    |   [loop C: i=0..4, insert 128+i]       |               |
    |               |                       |               |
    |         filter.record(1,128+i) ------>|               |
    |               |                       |--write[i]---->| OVERWRITE slot i
    |               |                       | (old id=i evicted)            |
    |               |                       | m_next=i+1    |               |
    |               |                       | m_count=128 (no change)       |
    |               |<----------------------|               |
    |   [end loop C: m_next=5]               |               |
    |               |                       |               |
    |   [loop D: i=0..4, check evicted]      |               |
    |               |                       |               |
    |         filter.is_duplicate(1,i) ---->|               |
    |               |                       |--scan[0..127]->| no match (id gone)
    |               |                       |<--false--------|
    |               |<--false---------------|               |
    |         assert(false) pass            |               |
    |   [end loop D]                         |               |
    |               |                       |               |
    |   [loop E: i=5..132, check survivors]  |               |
    |               |                       |               |
    |         filter.is_duplicate(1,i) ---->|               |
    |               |                       |--scan[0..127]->| match found
    |               |                       |<--true---------|
    |               |<--true----------------|               |
    |         assert(true) pass             |               |
    |   [end loop E]                         |               |
    |               |                       |               |
    |<--return true--|                        |               |
    |  printf(PASS)  |                        |               |


## 13. Initialization vs Runtime Flow

Initialization phase:
  filter.init() -- zeroes m_window[128], sets m_next=0, m_count=0.
  This is the only phase where memset is called.  No dynamic allocation.

Runtime phase (all loops A-E):
  Phase A: 128 record() calls -- fills the window.
  Phase B: 128 is_duplicate() calls -- verification pass.
  Phase C: 5 record() calls -- triggers eviction by overwriting oldest slots.
  Phase D: 5 is_duplicate() calls -- confirms eviction.
  Phase E: 128 is_duplicate() calls -- confirms survivors.

The round-robin eviction is implicit in record():
  There is no explicit "evict" function or "age" tracking.
  Eviction is a natural consequence of the ring buffer: the next write slot is always
  the oldest written slot (when the buffer is full).

Complexity:
  Phase A: O(128) record() calls, each O(1).
  Phase B: O(128) is_duplicate() calls, each O(128) scan -> O(16384) total comparisons.
  Phase C: O(5) record() calls, each O(1).
  Phase D: O(5) is_duplicate() calls, each O(128) scan -> O(640) comparisons.
  Phase E: O(128) is_duplicate() calls, each O(128) scan -> O(16384) comparisons.

Total: approximately 33,408 Entry comparisons.  All on-stack data; cache-friendly.


## 14. Known Risks / Observations

Risk 1 -- O(N) linear scan in is_duplicate():
  Every is_duplicate() call scans all 128 slots regardless of m_count or entry validity.
  With DEDUP_WINDOW_SIZE=128 this is acceptable.  If the constant is increased, the
  O(N^2) behavior in Phase B and Phase E becomes a performance concern.

Risk 2 -- Implicit eviction semantics (no age tracking):
  The ring buffer evicts the "oldest written" slot, not the "least recently seen" or
  "semantically oldest by timestamp".  This means a very old message that was the
  first recorded will be evicted exactly DEDUP_WINDOW_SIZE writes later, regardless
  of how recently it was queried.  In adversarial scenarios, a sender could re-send
  a message after DEDUP_WINDOW_SIZE intervening messages to bypass dedup.

Risk 3 -- m_count does not decrease on eviction:
  When record() overwrites a slot at capacity, m_count stays at 128.  There is no
  decrement.  m_count therefore represents "the window has been filled at some point"
  rather than "the number of currently unique entries".  The actual number of valid
  entries is always exactly 128 once the window is full.
  [OBSERVATION] m_count is used in assertions (assert(m_count <= 128)) but never used
  as a loop bound in is_duplicate().  is_duplicate() always scans all 128 slots.
  m_count is therefore partially redundant after fill-time; it does not break
  correctness but its semantic role is limited to the pre-fill phase.

Risk 4 -- No separate "invalid" overwrite flag:
  When a slot is overwritten, its valid flag stays true (it had true, it stays true
  after the write).  There is no window that marks the old entry as "evicting".
  This is correct; the old entry simply becomes the new entry atomically.

Risk 5 -- Phase E loop bound:
  for (uint32_t i = 5U; i < DEDUP_WINDOW_SIZE + 5U; ++i)
  i runs from 5 to 132 inclusive (128 iterations).
  DEDUP_WINDOW_SIZE + 5U = 128 + 5 = 133 as a uint32_t.  No overflow risk.
  [OBSERVATION] The loop correctly covers both the original survivors (msg_ids 5..127,
  in slots 5..127) and the newly inserted entries (msg_ids 128..132, in slots 0..4).

Risk 6 -- sizeof(Entry) alignment:
  If the compiler packs Entry differently than assumed (e.g., on a 32-bit target with
  4-byte alignment, sizeof(Entry) might be 16 bytes or 13 bytes depending on pragmas),
  memset size is still correct because sizeof(m_window) is computed by the compiler
  at compile time.  No user-provided size calculation.


## 15. Unknowns / Assumptions

[ASSUMPTION 1] DuplicateFilter has no user-provided constructor.  Automatic storage
  duration means fields are indeterminate before init().  init() is always called first.

[ASSUMPTION 2] sizeof(Entry) >= 13 bytes due to {NodeId(4), uint64_t(8), bool(1)}.
  With natural alignment, sizeof(Entry) == 16 bytes.  The memset covers the full
  array regardless of exact size because sizeof(m_window) is compiler-computed.

[ASSUMPTION 3] memset on an array of structs with bool members produces valid
  zero-initialisation (valid=false maps to 0x00).  This is standard for POD types.

[ASSUMPTION 4] The test binary is compiled without NDEBUG so assert() is active.

[ASSUMPTION 5] DEDUP_WINDOW_SIZE = 128 as declared in Types.hpp:21.  The test uses
  this constant directly (not a magic number), so any change to the constant would
  correctly scale the test.

[ASSUMPTION 6] NodeId is uint32_t (confirmed in Types.hpp:96).  Source ID 1U fits
  without truncation.

[ASSUMPTION 7] The ring-buffer eviction policy is intentionally FIFO (oldest-first),
  not LRU or any other policy.  The code provides no evidence of any other policy.

[ASSUMPTION 8] check_and_record() is not used in this test; only record() and
  is_duplicate() are called directly.  The test exercises the lower-level primitives
  to isolate the eviction behavior from the combined check-and-record logic.
```

---

Three documents produced. Key findings:

**UC_31 (PRNG Reproducibility):** The actual test `test_prng_deterministic()` validates determinism indirectly — it confirms both engines received the same seed (12345) via config equality and that both are in the disabled default state. It does not call `next()` or compare output sequences directly because `PrngEngine` is private to `ImpairmentEngine`. A notable latent risk: `PrngEngine::next_double()` asserts `result < 1.0` but `xorshift64` can theoretically produce `UINT64_MAX`, which would yield exactly `1.0` after the division and trigger `abort()`.

**UC_32 (100% Loss):** The correctness argument rests on `next_double()` always returning a value in `[0.0, 1.0)`, so the test `loss_rand < 1.0` is always true. The drop path returns `ERR_IO` before any delay-buffer insertion code is reached, leaving `m_delay_buf` completely untouched. `collect_deliverable()` then scans all 32 slots (all inactive) and returns 0. The same `UINT64_MAX` edge case identified in UC_31 applies here.

**UC_33 (Dedup Window Eviction):** Eviction is purely implicit — `record()` overwrites the slot at `m_next` with no explicit invalidation of the old entry. After filling 128 slots `m_next` wraps to 0, and the 5 additional writes overwrite slots 0-4. `is_duplicate()` performs a full linear O(128) scan every call; there is no hash or bitmap optimization. `m_count` saturates at 128 and never decrements, making it a fill-indicator rather than a true live-entry counter.agentId: aaef30bfc4d55e28d (for resuming to continue this agent's work if needed)
<usage>total_tokens: 58516
tool_uses: 10
duration_ms: 373421</usage>