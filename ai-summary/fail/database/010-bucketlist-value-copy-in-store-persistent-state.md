# H010: Avoid Value-Copy of BucketList in storePersistentStateAndLedgerHeaderInDB

**Date**: 2026-04-10
**Subsystem**: database, ledger
**Severity**: Informational
**Impact**: reduced heap allocations
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`storePersistentStateAndLedgerHeaderInDB` should pass the BucketList by
const reference to the `HistoryArchiveState` constructor, avoiding unnecessary
copies. Since both `getLiveBucketList()` and `getHotArchiveBucketList()`
return references and the `HistoryArchiveState` constructor already accepts
`const&`, there is no need for an intermediate copy.

## Mechanism

Line 2906 copies the LiveBucketList by value:
```cpp
LiveBucketList bl = mApp.getBucketManager().getLiveBucketList();
```
Line 2915 copies the HotArchiveBucketList:
```cpp
auto hotBl = mApp.getBucketManager().getHotArchiveBucketList();
```

Each BucketList has 11 levels. Each level copies:
- 2 `shared_ptr<Bucket>` (atomic refcount increment ~15ns each)
- 1 `FutureBucket` with up to 4 `shared_ptr`s, 4 hash strings (64-char
  hex, requiring heap allocation), 2 vectors

Total per copy: up to ~66 heap allocations for string/vector members.
Two BucketList copies: ~132 heap allocations per ledger close.

## Trigger

Every ledger close in the apply-load benchmark.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2906` — `LiveBucketList bl = ...` value copy
- `src/ledger/LedgerManagerImpl.cpp:2915` — `auto hotBl = ...` value copy
- `src/history/HistoryArchive.cpp:530-546` — constructor already takes `const&`

## Evidence

The `HistoryArchiveState` constructor signature is:
```cpp
HistoryArchiveState(uint32_t ledgerSeq, LiveBucketList const& buckets, ...)
```
The `const&` parameter means the constructor never modifies the BucketList.
The intermediate copy is unnecessary.

## Anti-Evidence

With a modern allocator (jemalloc/tcmalloc), 132 small heap allocations
take ~4-13μs. Over 200 benchmark ledgers: 0.8-2.6ms out of ~40,000ms total
(0.006%). This is 3 orders of magnitude below the 5% threshold for Low
severity and completely unmeasurable.

Furthermore, most FutureBucket fields in a well-resolved BucketList are
in FB_CLEAR or FB_LIVE_OUTPUT state, where the strings and shadow vectors
are empty — making the copies even cheaper (small-string optimization,
empty vector = no allocation).

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The value copy of BucketList costs approximately 10-20μs per ledger close
(~4ms total over 200 benchmark ledgers). This is 0.01% of total benchmark
time — three orders of magnitude below measurable threshold. While the
copy IS technically unnecessary (a `const auto&` would suffice), the
performance impact is negligible.

### Lesson Learned

BucketList objects appear large (11 levels × FutureBucket with many fields)
but their copy cost is dominated by shared_ptr atomic increments (~15ns)
and small strings (mostly under SSO threshold or empty). Always estimate
actual allocation and memcpy costs before proposing copy-avoidance
optimizations. In this case, the entire storePersistentStateAndLedgerHeaderInDB
function costs ~0.5-2ms per ledger, and the BucketList copy is <2% of
even THAT already-tiny cost.
