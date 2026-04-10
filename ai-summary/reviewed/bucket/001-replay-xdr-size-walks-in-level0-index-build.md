# H001: Level-0 index build replays XDR sizing work after the bucket was already written

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: ledger-close serial CPU and memory bandwidth
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When `mergeInMemory()` has already serialized the merged level-0 bucket, hashed
it, and tracked exact output byte counts, the follow-on in-memory index build
should reuse that write-time metadata. The hot close path should not immediately
re-walk every merged `BucketEntry` and recompute serialized sizes just to derive
`BucketEntryCounters` and type-offset metadata for the same bucket.

## Mechanism

`LiveBucket::mergeInMemory()` writes every merged entry through
`BucketOutputIterator`, then `getBucket()` constructs `LiveBucketIndex` from the
same in-memory vector. The `InMemoryIndex` constructor replays that vector and
does two size walks per entry: `BucketEntryCounters::count()` calls
`xdr::xdr_size(be)`, and the constructor calls `xdr::xdr_size(be)` again to
advance `lastOffset`. For Soroban-heavy ledgers, those repeated recursive XDR
size traversals run on the main thread every ledger even though the exact byte
positions were already known while the bucket was being written.

## Trigger

Run any write-heavy apply-load scenario (`sac`, `custom_token`, or `soroswap`)
with enough modified entries that level 0 contains thousands of merged
`CONTRACT_DATA`, `CONTRACT_CODE`, ACCOUNT, and TRUSTLINE entries. The overhead
appears on every ledger because level 0 is rebuilt on every close.

## Target Code

- `src/bucket/LiveBucket.cpp:549-612` — `mergeInMemory()` writes the merged output, then hands the same vector to `getBucket(...)`
- `src/bucket/BucketOutputIterator.cpp:152-179,224-227` — write path already tracks exact serialized bytes via `mBytesPut`, then constructs `LiveBucketIndex` from `inMemoryState`
- `src/bucket/LiveBucketIndex.cpp:84-88` — in-memory-state constructor always builds `InMemoryIndex`
- `src/bucket/InMemoryIndex.cpp:78-117` — constructor replays the full vector and recomputes offsets with `xdr::xdr_size(be)`
- `src/bucket/BucketUtils.cpp:327-337` — `BucketEntryCounters::count()` independently calls `xdr::xdr_size(be)` again

## Evidence

The level-0 path already paid for a full write pass through `BucketOutputIterator`
before `InMemoryIndex` starts. Despite that, `InMemoryIndex` rebuilds counters,
type ranges, and offsets by scanning the whole `inMemoryState` vector again, and
it invokes `xdr::xdr_size(be)` twice per entry in that second pass. Unlike the
file-backed index path, which uses actual file positions from `in.pos()`, the
level-0 in-memory path reconstructs byte offsets from scratch.

## Anti-Evidence

The index still needs some post-merge metadata (`AssetPoolIDMap`,
`BucketEntryCounters`, type ranges), so not all of the constructor pass is
avoidable. If the merged entries are mostly small classic objects rather than
large Soroban values, the savings may stay below the benchmark noise floor.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated. Related but distinct from: H002 (heap allocation per entry in InMemoryBucketState::insert), H001-inline (file-backed rescan elimination), H001-single-pass (two-pass merge structure in mergeInMemory).

### Trace Summary

Traced the full level-0 close path: `addBatchInternal` → `prepareFirstLevel` → `LiveBucket::mergeInMemory` → `BucketOutputIterator::put()` (which calls `XDROutputFileStream::writeOne` → `xdr::xdr_size(t)` once per entry for serialization) → `getBucket()` → `LiveBucketIndex(bm, *inMemoryState, mMeta)` → `InMemoryIndex` vector constructor. Confirmed the constructor calls `xdr::xdr_size(be)` twice per entry: once in `processEntry` → `BucketEntryCounters::count()` (BucketUtils.cpp:336) for entry size tracking, and once at line 112 for offset advancement. These two calls compute the identical value on the identical input and could trivially be combined.

### Code Paths Examined

- `src/bucket/BucketListBase.cpp:196-238` (`prepareFirstLevel`) — Confirmed level 0 always takes the in-memory merge path when `curr->hasInMemoryEntries()` is true.
- `src/bucket/LiveBucket.cpp:549-613` (`mergeInMemory`) — Produces `mergedEntries` vector, writes to disk via `BucketOutputIterator::put()`, then calls `getBucket(bucketManager, nullptr, make_unique<vector>(move(mergedEntries)))`.
- `src/bucket/BucketOutputIterator.cpp:168-230` (`getBucket`) — Constructs `LiveBucketIndex(bm, *inMemoryState, mMeta)` when no existing index found.
- `src/bucket/InMemoryIndex.cpp:78-117` — **The duplication site**: Loop at line 105-113 calls `processEntry(be, ...)` which internally calls `counters.count<LiveBucket>(be)` → `xdr::xdr_size(be)` at BucketUtils.cpp:336, then line 112 calls `xdr::xdr_size(be)` again for `lastOffset` advancement.
- `src/bucket/BucketUtils.cpp:325-337` (`BucketEntryCounters::count`) — Confirmed `xdr::xdr_size(be)` call at line 336 for `entryTypeSizes` accumulation.
- `lib/xdrpp/xdrpp/types.h:222-227` (`xdr_size`) — Dispatches to `xdr_traits<T>::serial_size(t)`. For variable-size structs (BucketEntry → LedgerEntry → ContractDataEntry → SCVal), this recursively walks all XDR fields. Not a constant-time operation — cost scales with entry complexity.
- `src/util/XDRStream.h:483-486` (`writeOne`) — Also calls `xdr::xdr_size(t)` (line 486), making this the THIRD computation of the same value in the overall level-0 path per entry (write pass + 2× in InMemoryIndex). However, the write-path value is not easily passable to the index constructor.

### Findings

The inefficiency is confirmed. Within the `InMemoryIndex` vector constructor alone, `xdr::xdr_size(be)` is computed twice per entry for the same value:

1. **In `processEntry` → `BucketEntryCounters::count()`** (BucketUtils.cpp:336): `entryTypeSizes[ledt] += xdr::xdr_size(be)` — computes size for per-type size accounting.
2. **In the main loop** (InMemoryIndex.cpp:112): `lastOffset += xdr::xdr_size(be) + xdrOverheadBetweenEntries` — computes the same size to advance the virtual file offset.

The fix is trivial: compute `auto entrySize = xdr::xdr_size(be)` once per entry and pass it to both consumers. This requires either (a) adding a `countWithSize()` method to `BucketEntryCounters` that accepts a pre-computed size, or (b) refactoring `processEntry` to compute size first and use it for both purposes.

**Severity downgraded from Low to Informational because:**

- `xdr_size` for typical BucketEntry types is fast (50-200ns) due to heavy template inlining and mostly fixed-size subfields. SCVal keys and values in SAC/token workloads are typically shallow (Symbol, i128, small Vec), keeping per-call cost at the lower end.
- Level 0 entry count per ledger: ~3000-15000 entries for heavy Soroban workloads (up to ~50000 near spill time with 4 accumulated ledgers).
- Savings from eliminating one xdr_size call: 3000-15000 × 50-200ns = 150μs - 3ms per ledger.
- Against a 30-50ms ledger close, this is ~0.3-6%. The typical case (10000 entries × 100ns = 1ms against 30ms = 3.3%) falls below the 5% Low threshold.
- Near spill time with accumulated entries, savings could approach 5%, but this happens only once every 4 ledgers, so the amortized impact is lower.

The fix is unambiguously correct: it's pure memoization of a pure function with no thread safety, ownership, or API contract concerns.

### PoC Guidance

- **Target code**: `src/bucket/InMemoryIndex.cpp` (constructor at line 78, loop at lines 105-113), `src/bucket/BucketUtils.h` and `src/bucket/BucketUtils.cpp` (`BucketEntryCounters::count` template)
- **Change description**: In the `InMemoryIndex` constructor's loop body, compute `auto entrySize = xdr::xdr_size(be)` once. Add a `countWithSize()` method to `BucketEntryCounters` (or modify `processEntry` to accept a pre-computed size) and use it for the `entryTypeSizes` accumulation. Use the same `entrySize` for the `lastOffset` advancement at line 112. This eliminates one `xdr_size` recursive walk per entry.
- **Correctness check**: Run `[bucket]` and `[bucketindex]` test tags. The `InMemoryIndex` equality operator (line 174-179) can verify that the modified constructor produces identical results. Also run `[bucketlist]` tests to ensure level-0 merge outputs are unchanged.
- **Benchmark focus**: Run `sac` and `custom_token` benchmarks at TX=3200, T=1. Measure per-ledger close time. Expect <3% improvement in median close time — likely within noise for most configurations but measurable with Tracy-level profiling of the `InMemoryIndex` constructor (which has a `ZoneScoped` at line 82).
