# UC_33 — Dedup sliding-window eviction test

**HL Group:** HL-6 — Duplicate Message Suppression
**Actor:** System
**Requirement traceability:** REQ-3.2.6, REQ-3.3.3

---

## 1. Use Case Overview

### What triggers this flow

The test function test_window_wraparound() in tests/test_DuplicateFilter.cpp exercises
the DuplicateFilter ring-buffer's eviction behavior when the window overflows.
It is a white-box test that verifies three properties simultaneously:
  1. record() correctly evicts the oldest entry (round-robin FIFO) when the window
     is at capacity.
  2. An evicted entry is no longer detected as a duplicate by is_duplicate().
  3. Non-evicted entries (both original fills and newly inserted entries) are still
     correctly detected as duplicates.

The test is triggered by main() in tests/test_DuplicateFilter.cpp.

### Expected outcome (single goal)

After filling the window with 128 unique entries (DEDUP_WINDOW_SIZE), inserting 5 more
entries causes the oldest 5 to be evicted. The test confirms: evicted entries return
false from is_duplicate(), and all 128 currently-resident entries (5 new + 123 surviving
original) return true.

---

## 2. Entry Points

Primary entry point (process-wide):

    main()  [tests/test_DuplicateFilter.cpp:166]
      → calls test_window_wraparound()  [line 198]

Secondary entry point (the test function itself):

    test_window_wraparound()  [tests/test_DuplicateFilter.cpp:121]

Objects constructed on the test stack:

    DuplicateFilter filter  [line 123]
    No user-provided constructor exists (DuplicateFilter.hpp has no constructor
    declaration). Fields m_window[], m_next, m_count are indeterminate until
    filter.init() is called.

---

## 3. End-to-End Control Flow (Step-by-Step)

--- SETUP PHASE ---

Step 1  main() calls test_window_wraparound() [line 198].

Step 2  Stack allocation: DuplicateFilter filter [line 123].
  Fields are indeterminate before init(). sizeof(DuplicateFilter) ≈ 2056 bytes.

Step 3  filter.init() [line 124 → DuplicateFilter.cpp:23].
  (void)memset(m_window, 0, sizeof(m_window))
    sizeof(m_window) = 128 × 16 = 2048 bytes.
    [sizeof(Entry) = NodeId(4) + uint64_t(8) + bool(1) + 3 bytes padding = 16.]
    All 128 Entry slots zeroed: src=0, msg_id=0, valid=false.
    (void) cast suppresses MISRA warning on memset return value.
  m_next = 0U.
  m_count = 0U.
  NEVER_COMPILED_OUT_ASSERT(m_next == 0U).
  NEVER_COMPILED_OUT_ASSERT(m_count == 0U).

--- PHASE A: Fill window with 128 unique entries ---

Step 4  First loop [lines 128-131]:
  for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i)  // i = 0..127
    msg_id = static_cast<uint64_t>(i)
    filter.record(1U, msg_id)          [DuplicateFilter.cpp:63]

  Per call to record(1U, i):
    NEVER_COMPILED_OUT_ASSERT(m_next < 128)   [pre-condition]
    NEVER_COMPILED_OUT_ASSERT(m_count <= 128) [pre-condition]
    m_window[m_next].src    = 1U
    m_window[m_next].msg_id = i
    m_window[m_next].valid  = true
    m_next = (m_next + 1) % 128
    if m_count < 128: m_count++
    NEVER_COMPILED_OUT_ASSERT(m_next < 128)   [post-condition]
    NEVER_COMPILED_OUT_ASSERT(m_count <= 128) [post-condition]

  Critical iteration — i=127 (last fill):
    m_window[127] = {src=1, msg_id=127, valid=true}
    m_next = (127 + 1) % 128 = 0   ← WRAP: pointer returns to slot 0
    m_count = 127 < 128 → m_count = 128   ← count reaches maximum

  State after Phase A:
    m_window[i] = {src=1, msg_id=i, valid=true}  for i in [0, 127]
    m_next  = 0   (wrapped around after writing slot 127)
    m_count = 128 (window exactly full)

--- PHASE B: Verify all 128 entries are duplicates ---

Step 5  Second loop [lines 134-137]:
  for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i)
    assert(filter.is_duplicate(1U, static_cast<uint64_t>(i)) == true)

  Per call to is_duplicate(1U, i):
    [DuplicateFilter.cpp:40]
    Linear scan of all 128 slots.
    Per slot j: NEVER_COMPILED_OUT_ASSERT(j < 128)  [loop invariant]
    At slot j==i: valid=true, src=1, msg_id==i → match; return true.
    All 128 test assertions (standard assert()) pass.

--- PHASE C: Insert 5 more entries — evict oldest 5 ---

Step 6  Third loop [lines 140-143]:
  for (uint32_t i = 0U; i < 5U; ++i)
    msg_id = static_cast<uint64_t>(DEDUP_WINDOW_SIZE) + static_cast<uint64_t>(i)
           = 128ULL + i   // 128, 129, 130, 131, 132
    filter.record(1U, msg_id)

  State entering Phase C: m_next = 0, m_count = 128.

  Iteration i=0: record(1U, 128ULL)
    NEVER_COMPILED_OUT_ASSERT(m_next=0 < 128)     passes
    NEVER_COMPILED_OUT_ASSERT(m_count=128 <= 128)  passes
    m_window[0].src    = 1U    (OVERWRITES slot 0; was {1, 0, true})
    m_window[0].msg_id = 128ULL
    m_window[0].valid  = true  (was already true; no state gap during overwrite)
    m_next = 1
    m_count == 128 (== DEDUP_WINDOW_SIZE) → NOT incremented [line 80 branch not taken]
    EVICTION: entry (src=1, msg_id=0) is permanently gone from the window.

  Iteration i=1: record(1U, 129ULL)
    m_window[1] overwritten: {1, 1, true} → {1, 129, true}
    m_next = 2; m_count stays 128.
    EVICTION: (src=1, msg_id=1) gone.

  Iteration i=2: record(1U, 130ULL)
    m_window[2] overwritten: {1, 2, true} → {1, 130, true}
    m_next = 3; m_count stays 128.
    EVICTION: (src=1, msg_id=2) gone.

  Iteration i=3: record(1U, 131ULL)
    m_window[3] overwritten: {1, 3, true} → {1, 131, true}
    m_next = 4; m_count stays 128.
    EVICTION: (src=1, msg_id=3) gone.

  Iteration i=4: record(1U, 132ULL)
    m_window[4] overwritten: {1, 4, true} → {1, 132, true}
    m_next = 5; m_count stays 128.
    EVICTION: (src=1, msg_id=4) gone.

  State after Phase C:
    Slots 0-4:   {src=1, msg_id=128..132, valid=true}  (overwritten)
    Slots 5-127: {src=1, msg_id=5..127, valid=true}    (unchanged from Phase A)
    m_next  = 5
    m_count = 128

--- PHASE D: Verify evicted entries are no longer duplicates ---

Step 7  Fourth loop [lines 147-151]:
  for (uint32_t i = 0U; i < 5U; ++i)
    is_dup = filter.is_duplicate(1U, static_cast<uint64_t>(i))
    assert(is_dup == false)

  For is_duplicate(1U, 0ULL):
    Linear scan of all 128 slots:
      Slot 0:  msg_id=128 != 0  → no match
      Slot 1:  msg_id=129 != 0  → no match
      Slot 2:  msg_id=130 != 0  → no match
      Slot 3:  msg_id=131 != 0  → no match
      Slot 4:  msg_id=132 != 0  → no match
      Slots 5-127: msg_id=5..127, none == 0  → no match
    Loop exhausted; NEVER_COMPILED_OUT_ASSERT(m_count<=128) passes; return false.
  assert(false == false) → pass.

  Same outcome for msg_ids 1, 2, 3, 4. All 5 assertions pass.

--- PHASE E: Verify surviving entries (msg_id 5-132) are still duplicates ---

Step 8  Fifth loop [lines 154-158]:
  for (uint32_t i = 5U; i < DEDUP_WINDOW_SIZE + 5U; ++i)  // i = 5..132
    is_dup = filter.is_duplicate(1U, static_cast<uint64_t>(i))
    assert(is_dup == true)

  For i=5..127 (msg_ids 5..127):
    Each found in slots 5..127 respectively (unchanged from Phase A).
    is_duplicate returns true; assert passes.

  For i=128..132 (msg_ids 128..132):
    Each found in slots 0..4 respectively (written in Phase C).
    is_duplicate returns true; assert passes.

  All 128 assertions pass.
  [128 = 123 survivors from Phase A (slots 5..127) + 5 new entries (slots 0..4).]

Step 9  return true [line 160].

Step 10  main() receives true; condition !true is false → else-branch taken:
  printf("PASS: test_window_wraparound\n")

---

## 4. Call Tree (Hierarchical)

main()  [tests/test_DuplicateFilter.cpp:166]
  test_window_wraparound()  [line 121]
    filter.init()  [DuplicateFilter.cpp:23]
      (void)memset(m_window, 0, 2048)           -- CRT; zeroes all 128 slots
      NEVER_COMPILED_OUT_ASSERT(m_next == 0U)   -- post-condition [line 32]
      NEVER_COMPILED_OUT_ASSERT(m_count == 0U)  -- post-condition [line 33]
    [Phase A loop ×128]
      filter.record(1U, msg_id)  [DuplicateFilter.cpp:63]
        NEVER_COMPILED_OUT_ASSERT(m_next < 128)  [pre-condition, line 66]
        NEVER_COMPILED_OUT_ASSERT(m_count <= 128)[pre-condition, line 67]
        (write slot at m_next; advance m_next modulo 128; increment m_count if < 128)
        NEVER_COMPILED_OUT_ASSERT(m_next < 128)  [post-condition, line 85]
        NEVER_COMPILED_OUT_ASSERT(m_count <= 128)[post-condition, line 86]
    [Phase B loop ×128]
      filter.is_duplicate(1U, msg_id)  [DuplicateFilter.cpp:40]
        [inner loop j=0..127]
          NEVER_COMPILED_OUT_ASSERT(j < 128)    [loop invariant, line 46]
          (compare slot; return true on match)
        NEVER_COMPILED_OUT_ASSERT(m_count <= 128)[post-loop, line 54]
        → return true (match at slot j==i)
    [Phase C loop ×5]
      filter.record(1U, 128+i)  [DuplicateFilter.cpp:63]
        (overwrite slot at m_next=0..4; m_next advances to 1..5; m_count unchanged at 128)
        → FIFO eviction of oldest entries at slots 0..4
    [Phase D loop ×5]
      filter.is_duplicate(1U, i)  [DuplicateFilter.cpp:40]
        (linear scan 128 slots; no match for evicted msg_ids 0..4; return false)
    [Phase E loop ×128]
      filter.is_duplicate(1U, i)  [DuplicateFilter.cpp:40]
        (linear scan 128 slots; match found for msg_ids 5..132; return true)
    return true
  [if !true → false → else branch]
  printf("PASS: test_window_wraparound\n")

---

## 5. Key Components Involved

Component                    File                                  Role
─────────────────────────────────────────────────────────────────────────────
test_window_wraparound()     tests/test_DuplicateFilter.cpp:121   Test driver;
                                                                  exercises 5
                                                                  phases.

DuplicateFilter              src/core/DuplicateFilter.hpp:29      Ring-buffer
                             src/core/DuplicateFilter.cpp         dedup class;
                                                                  init/record/
                                                                  is_duplicate.

DuplicateFilter::Entry       src/core/DuplicateFilter.hpp:54      POD struct:
                                                                  {NodeId src,
                                                                  uint64_t msg_id,
                                                                  bool valid};
                                                                  16 bytes.

DuplicateFilter::m_next      DuplicateFilter.hpp:62               Write pointer
                                                                  (uint32_t, 0..127);
                                                                  wraps via modulo.
                                                                  The ring-buffer
                                                                  cursor; determines
                                                                  which slot is
                                                                  overwritten next.

DuplicateFilter::m_count     DuplicateFilter.hpp:63               Entry count
                                                                  (uint32_t, 0..128);
                                                                  capped at 128;
                                                                  not decremented
                                                                  on eviction.

NEVER_COMPILED_OUT_ASSERT    src/core/Assert.hpp                  Always-active
                                                                  assertion macro;
                                                                  not NDEBUG-gated.

Types.hpp                    src/core/Types.hpp:23                DEDUP_WINDOW_SIZE
                                                                  = 128U;
                                                                  NodeId = uint32_t.

---

## 6. Branching Logic / Decision Points

Decision 1 — record() line 80: if (m_count < DEDUP_WINDOW_SIZE)
  Phase A (i=0..127): m_count < 128 for all iterations → True: m_count incremented.
  Phase C (i=0..4): m_count == 128 → False: NOT incremented (at capacity).
  This is the eviction gate. The old slot at m_next is simply overwritten with
  the new data without first clearing it. No `valid = false` is set; the
  replacement is atomic within the single record() call.

Decision 2 — is_duplicate() line 48: linear scan match condition
  if (m_window[j].valid && m_window[j].src==src && m_window[j].msg_id==msg_id)
  Phase B: match at slot j==i for all i → early return true.
  Phase D: no slot contains evicted msg_ids 0..4 → loop exhausts all 128 → return false.
  Phase E: match found for each queried msg_id in slots 0..4 (new) or 5..127 (original).

Decision 3 — m_next modulo wrap: (m_next + 1U) % DEDUP_WINDOW_SIZE  [record() line 76]
  After filling 128 entries, m_next wrapped from 127 back to 0.
  Phase C writes start at slot 0, advancing to slot 5 after 5 insertions.
  This implements round-robin (oldest-first) FIFO eviction.

Decision 4 — main() return check: if (!test_window_wraparound())
  Returns true → condition false → else-branch taken → "PASS" printed.
  Any failing assertion within test_window_wraparound() calls abort() before
  the function can return false.

---

## 7. Concurrency / Threading Behavior

Single-threaded. DuplicateFilter has no mutexes, no std::atomic members, and no
thread creation.

m_next (uint32_t) and m_count (uint32_t) are plain non-atomic; modified only by
record(). m_window[] entries are modified only by record() and read only by
is_duplicate(). All operations are sequentially ordered within
test_window_wraparound().

In a production multi-threaded context, DuplicateFilter would require external
synchronization before calling record() and is_duplicate() from multiple threads.
The class does not document or enforce thread safety; callers bear full responsibility.
No multi-thread scenario is exercised by this test.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

DuplicateFilter members (all value types, no pointers):

    Entry    m_window[128]  — fixed array; 2048 bytes; no dynamic allocation
    uint32_t m_next         — 4 bytes
    uint32_t m_count        — 4 bytes
    Total: ~2056 bytes on the stack of test_window_wraparound().

sizeof(Entry) = 16 bytes:
  NodeId(uint32_t)=4 + uint64_t=8 + bool=1 + 3 bytes compiler padding
  (natural 8-byte alignment for the uint64_t member).
  sizeof(m_window) = 16 × 128 = 2048 bytes. Compiler-computed; (void)memset correct.

(void)memset in init() [line 26]:
  Zeroes the entire m_window array including padding bytes.
  For POD types (no vtables, no non-trivial constructors), 0x00 initializes
  src=0, msg_id=0, valid=false. Standard-conforming for C++ POD types.

record() writes directly into m_window[m_next]:
  Pure field assignment (lines 70-72); no allocation, no memcpy.
  Overwrites whatever was previously at that slot (implicit eviction).
  The old (src, msg_id) is not saved; it is gone after the write.
  The valid field is set to true; it was already true on eviction. There is no
  transient window where the slot shows valid=false.

is_duplicate() [declared const, line 38]:
  Reads m_window[0..127] sequentially; no writes. Correct const qualification.

Ownership:
  DuplicateFilter is on the stack of test_window_wraparound().
  Lifetime: from declaration [line 123] to function return [line 160].
  No pointers to m_window or its entries escape the object.
  No destructor needed (no resources to release).

---

## 9. Error Handling Flow

No error codes are returned from record() (void) or is_duplicate() (bool).
check_and_record() (which returns Result) is NOT called in test_window_wraparound().
Only record() and is_duplicate() are used directly, isolating eviction behavior from
the combined check-and-record logic.

NEVER_COMPILED_OUT_ASSERT failure modes (always active, not NDEBUG-gated):
  record() line 66: m_next >= 128 → impossible (modulo arithmetic always yields 0..127).
  record() line 67: m_count > 128 → impossible (capped at 128; never incremented beyond).
  is_duplicate() line 46: j >= 128 → impossible (loop condition is `j < 128`).
  is_duplicate() line 54: m_count > 128 → same bound as above; always passes.

Test assertion failures (standard assert() in test code, per tests/ exemption):
  Phase B: assert(is_dup == true) — fires if a newly recorded entry is not found.
    Indicates a bug in record() or is_duplicate() search logic.
  Phase D: assert(is_dup == false) — fires if an evicted entry is still found.
    Indicates the overwrite in record() did not correctly replace the old key.
  Phase E: assert(is_dup == true) — fires if a surviving or new entry is not found.
    Indicates data corruption in m_window or logic error in is_duplicate().

All assertion failures call abort() via the NEVER_COMPILED_OUT_ASSERT path or via
standard assert(); no exceptions; no Result codes. The test is binary: all phases
pass or one assertion aborts the process.

---

## 10. External Interactions

No network, filesystem, or OS calls anywhere on this path.

CRT interactions:
  memset()  — called once during filter.init() [DuplicateFilter.cpp:26].

Test code external output:
  assert()  — standard assert() used in test driver; calls abort() on failure.
  printf()  — called in main() after the test returns true; writes to stdout.
  Not called inside test_window_wraparound() itself.

No Logger calls, no socket operations, no timer queries.

---

## 11. State Changes / Side Effects

State of filter after init():
  m_window[0..127]: all {src=0, msg_id=0, valid=false}
  m_next  = 0
  m_count = 0

State after Phase A (128 record() calls):
  m_window[i] = {src=1, msg_id=i, valid=true}  for i in [0, 127]
  m_next  = 0  (wrapped around after writing slot 127)
  m_count = 128

State after Phase B (128 is_duplicate() calls):
  Unchanged (is_duplicate() is const; no writes).

State after Phase C (5 record() calls — eviction):
  Slot 0:   {src=1, msg_id=128, valid=true}  (was {1, 0, true})
  Slot 1:   {src=1, msg_id=129, valid=true}  (was {1, 1, true})
  Slot 2:   {src=1, msg_id=130, valid=true}  (was {1, 2, true})
  Slot 3:   {src=1, msg_id=131, valid=true}  (was {1, 3, true})
  Slot 4:   {src=1, msg_id=132, valid=true}  (was {1, 4, true})
  Slots 5-127: unchanged {src=1, msg_id=5..127, valid=true}
  m_next  = 5
  m_count = 128  (unchanged; was already at cap)

State after Phases D and E:
  Unchanged (all is_duplicate() calls are const reads).

Side effects visible outside the test:
  stdout: "PASS: test_window_wraparound\n" (from main() line 201).
  No other side effects; no stderr output; no OS resources consumed.

---

## 12. Sequence Diagram (ASCII)

```
  main()   test_window_wraparound()   DuplicateFilter      Entry[128]
    |               |                       |                    |
    |--test_wrap()-->                        |                    |
    |               |                       |                    |
    |         filter.init() [cpp:23]------->|                    |
    |               |                       | (void)memset------>| all valid=false
    |               |                       | m_next=0           |
    |               |                       | m_count=0          |
    |               |                       | NCOA(next==0)      |
    |               |                       | NCOA(count==0)     |
    |               |<----------------------|                    |
    |               |                       |                    |
    |   [Phase A: loop i=0..127]             |                    |
    |               |                       |                    |
    |         filter.record(1,i)----------->|                    |
    |               |                       | NCOA(next<128)     |
    |               |                       | NCOA(cnt<=128)     |
    |               |                       |--write[i]--------->| valid=T,id=i
    |               |                       | m_next=(i+1)%128   |
    |               |                       | m_count++ (if<128) |
    |               |                       | NCOA(next<128)     |
    |               |                       | NCOA(cnt<=128)     |
    |               |<----------------------|                    |
    |   [end Phase A: m_next=0, m_count=128]|                    |
    |               |                       |                    |
    |   [Phase B: loop i=0..127, assert true]                    |
    |         filter.is_dup(1,i)---------->|                    |
    |               |                       |--scan[0..127]----->| find slot i
    |               |                       | NCOA(j<128) ×128   |
    |               |                       | match at slot i    |
    |               |<--true----------------|                    |
    |         assert(true) pass             |                    |
    |   [end Phase B]                        |                    |
    |               |                       |                    |
    |   [Phase C: loop i=0..4, insert 128+i]|                    |
    |         filter.record(1,128+i)------->|                    |
    |               |                       |--write[i]--------->| OVERWRITE slot i
    |               |                       | (old msg_id=i gone)|
    |               |                       | m_next=i+1         |
    |               |                       | m_count=128(no chg)|
    |               |<----------------------|                    |
    |   [end Phase C: m_next=5]             |                    |
    |               |                       |                    |
    |   [Phase D: loop i=0..4, assert false]|                    |
    |         filter.is_dup(1,i)---------->|                    |
    |               |                       |--scan[0..127]----->| no match (evicted)
    |               |<--false---------------|                    |
    |         assert(false==false) pass     |                    |
    |   [end Phase D]                        |                    |
    |               |                       |                    |
    |   [Phase E: loop i=5..132, assert true]                    |
    |         filter.is_dup(1,i)---------->|                    |
    |               |                       |--scan[0..127]----->| match found
    |               |<--true----------------|                    |
    |         assert(true==true) pass       |                    |
    |   [end Phase E]                        |                    |
    |               |                       |                    |
    |<--return true--|                        |                    |
    |  printf(PASS)  |                        |                    |
```

NCOA = NEVER_COMPILED_OUT_ASSERT

---

## 13. Initialization vs Runtime Flow

Initialization phase:

  filter.init() [DuplicateFilter.cpp:23] — zeroes m_window[128] via (void)memset,
  sets m_next=0, m_count=0, fires two NEVER_COMPILED_OUT_ASSERT() post-conditions.
  This is the only phase where memset is called. No dynamic allocation at any phase.

Runtime phase (all loops A-E):

  Phase A: 128 record() calls — fills the window; m_next wraps from 127→0;
           m_count reaches 128.
  Phase B: 128 is_duplicate() calls — read-only verification; no state change.
  Phase C: 5 record() calls — triggers implicit eviction by overwriting slots 0..4.
           m_next advances from 0 to 5; m_count stays 128.
  Phase D: 5 is_duplicate() calls — confirms eviction; read-only.
  Phase E: 128 is_duplicate() calls — confirms all 128 survivors (slots 0..4 have new
           data; slots 5..127 have original data); read-only.

Round-robin eviction is implicit in record():
  There is no explicit "evict" function, "age" field, or "dirty" flag.
  Eviction is a natural consequence of the ring buffer: when the buffer is full,
  the next write position (m_next) is always the oldest-written slot.
  The overwrite is atomic within record(); there is no intermediate state where
  the slot is momentarily invalid.

Algorithmic complexity (total comparisons in this test):
  Phase A: 128 × O(1) record() calls.
  Phase B: 128 × O(128) is_duplicate() calls → ~16,384 Entry comparisons.
  Phase C: 5 × O(1) record() calls.
  Phase D: 5 × O(128) is_duplicate() calls → ~640 Entry comparisons.
  Phase E: 128 × O(128) is_duplicate() calls → ~16,384 Entry comparisons.
  Total: approximately 33,408 Entry comparisons. All on-stack data; cache-friendly.

---

## 14. Known Risks / Observations

Risk 1 — O(N) linear scan in is_duplicate()
  Every is_duplicate() call scans all 128 slots regardless of m_count or entry
  validity. With DEDUP_WINDOW_SIZE=128 this is acceptable. If the constant is
  increased, the O(N²) behavior across Phases B and E becomes a performance concern.
  See docs/WCET_ANALYSIS.md for the WCET analysis of check_and_record.

Risk 2 — Implicit eviction semantics (no age or timestamp tracking)
  The ring buffer evicts the "oldest written" slot, not "least recently queried"
  or "semantically oldest by timestamp." A sender that retransmits a message after
  exactly DEDUP_WINDOW_SIZE intervening messages has a chance of bypassing dedup
  silently if the eviction cycle coincides precisely.

Risk 3 — m_count does not decrease on eviction
  When record() overwrites a slot at capacity, m_count stays at 128 (the false branch
  at DuplicateFilter.cpp:80 is not taken). m_count represents "the window has been
  filled at some point" rather than "number of currently distinct entries."
  After the fill phase, m_count is partially redundant: is_duplicate() always scans
  all 128 slots regardless of m_count. m_count is used only in assertions
  (NEVER_COMPILED_OUT_ASSERT(m_count <= 128)) and never as a loop bound.

Risk 4 — No explicit eviction flag or valid-false gap during overwrite
  When a slot is overwritten, its valid flag stays true. There is no window where
  the old entry is marked "evicting" and the new one is not yet "valid." The
  replacement is atomic within record(). This is correct but means there is no
  transient state that could be observed by a concurrent is_duplicate() call
  (not that this matters in the single-threaded design).

Risk 5 — Phase E loop bound arithmetic
  for (uint32_t i = 5U; i < DEDUP_WINDOW_SIZE + 5U; ++i)
  DEDUP_WINDOW_SIZE + 5U = 128 + 5 = 133 as a uint32_t. No overflow risk (far below
  UINT32_MAX). Covers both original survivors (msg_ids 5..127, slots 5..127) and
  newly inserted entries (msg_ids 128..132, slots 0..4).

Risk 6 — sizeof(Entry) and memset correctness
  sizeof(m_window) is compiler-computed at compile time, so the memset size is always
  correct regardless of alignment or packing decisions. The (void) cast on memset
  correctly suppresses any MISRA warning about discarding the return value.

---

## 15. Unknowns / Assumptions

All previously-open assumptions have been resolved by direct source inspection.
The following facts are confirmed:

[CONFIRMED 1] DuplicateFilter has no user-provided constructor (DuplicateFilter.hpp has
  no constructor declaration). Fields are indeterminate before init(). The test always
  calls filter.init() before any other method.

[CONFIRMED 2] sizeof(Entry) is 16 bytes with natural 8-byte alignment on typical 64-bit
  ABI: NodeId(4) + uint64_t(8) + bool(1) + 3 bytes padding = 16. The memset covers the
  full array because sizeof(m_window) is compiler-computed.

[CONFIRMED 3] (void)memset on an array of POD structs with bool members produces valid
  zero-initialization: 0x00 maps to false for bool, and zero for integer types.
  Standard-conforming for C++ POD types.

[CONFIRMED 4] Internal assertions use NEVER_COMPILED_OUT_ASSERT() (from Assert.hpp),
  not the standard assert() macro. NEVER_COMPILED_OUT_ASSERT() is always active
  regardless of NDEBUG. Test-code assertions in test_DuplicateFilter.cpp use standard
  assert() from <cassert> (permitted in tests/ per CLAUDE.md §9 compliance table).

[CONFIRMED 5] DEDUP_WINDOW_SIZE = 128U, declared at Types.hpp:23. The test uses this
  constant directly, so any change to the constant scales the test correctly.

[CONFIRMED 6] NodeId is typedef uint32_t (Types.hpp:98). Source ID 1U fits without
  truncation.

[CONFIRMED 7] The ring-buffer eviction policy is intentionally FIFO (oldest-written-first).
  No LRU or timestamp-based policy exists. The code provides no evidence of any
  alternative eviction policy.

[CONFIRMED 8] check_and_record() [DuplicateFilter.cpp:93] is not called in this test.
  Only record() and is_duplicate() are exercised directly, isolating eviction behavior
  from the combined check-and-record logic.
