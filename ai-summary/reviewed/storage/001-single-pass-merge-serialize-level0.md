# H001: Single-Pass Merge + Serialize in Level-0 mergeInMemory

**Date**: 2025-07-22
**Subsystem**: storage (bucket)
**Severity**: Low
**Impact**: per-ledger CPU reduction in level-0 bucket write path
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `mergeInMemory` produces the level-0 bucket, each output entry should be
processed exactly once: merged from the two input buckets, serialized to XDR,
hashed into the running SHA256, written to the output stream, and accumulated
in the in-memory entries vector. The current implementation processes each
entry twice: first accumulating into an intermediate `mergedEntries` vector
via the merge loop, then iterating that vector again through
`BucketOutputIterator::put()` to serialize, hash, and write to disk.

## Mechanism

`LiveBucket::mergeInMemory` (LiveBucket.cpp:550-613) runs in two sequential
phases:

**Phase 1** (lines 584-591): `mergeInternal` calls `putFunc` for each merged
entry. The lambda copies the entry into `mergedEntries` via
`emplace_back(entry)` where `entry` is `BucketEntry const&` — a deep copy of
~200-500 bytes including heap-allocated XDR fields.

**Phase 2** (lines 599-606): A second loop iterates all `mergedEntries`, calling
`out.put(e)` for each. Inside `put()` (BucketOutputIterator.cpp:78-165):
- Key comparison with the buffered entry (`mCmp(*mBuf, e)`) — redundant since
  `mergeInternal` already deduplicated same-key entries
- Deep copy into the output buffer (`*mBuf = e`) — ~200-500 bytes per entry
- XDR serialization + SHA256 update + disk write via `writeOne(*mBuf, ...)`

By fusing Phases 1 and 2 into a single pass — having `putFunc` directly
serialize+hash+write each entry while also accumulating it — the optimization
eliminates:
- 10,000 loop iterations (Phase 2)
- 10,000 key comparisons in `put()` (~20-50ns each)
- 10,000 deep copies into `mBuf` (~100-300ns each for typical entries)
- Cache re-warming: in Phase 2, entries evicted from L1/L2 cache since Phase 1
  must be re-fetched (~50-100ns per miss for 300-byte entries)

## Trigger

Any Soroban-heavy ledger close. In the apply-load benchmark:
- SAC 3200 txs: ~16,000 entries per level-0 merge (every ledger)
- Custom token 3000 txs: ~15,000 entries
- Soroswap 1600 txs: ~8,000 entries
- Level 0 merges every ledger (spills to level 1 every 2 ledgers)

## Target Code

- `src/bucket/LiveBucket.cpp:mergeInMemory:549-613` — the two-phase structure: merge into `mergedEntries` (lines 584-591), then iterate through `BucketOutputIterator` (lines 599-606)
- `src/bucket/BucketOutputIterator.cpp:put:78-165` — buffer-based deduplication + serialize, redundant dedup for pre-merged input
- `src/bucket/BucketBase.cpp:mergeInternal:289-425` — the merge template that calls `putFunc`; currently instantiated with `std::function<void(BucketEntry const&)>` for the in-memory path (line 429-434)

## Evidence

1. The two-phase pattern is visible in `mergeInMemory`: Phase 1 builds `mergedEntries` (lines 584-591), Phase 2 iterates it (lines 603-606). This double iteration processes each entry twice.
2. `BucketOutputIterator::put()` does key comparison and buffering (lines 142-164) that is redundant for pre-deduplicated merge output — `mergeInternal` already handles all key conflicts via `mergeCasesWithEqualKeys`.
3. The review of `bucket/reviewed/001` (level-0 disk write deferral) explicitly identifies this as a "more impactful variant": "compute the hash during the merge in a single pass (serialize + hash each entry inline in the putFunc lambda instead of emitting to mergedEntries and then re-serializing through BucketOutputIterator)."
4. For 16,000 entries × (deep copy at ~200ns + key comparison at ~30ns + buffer copy at ~200ns + cache miss at ~50ns) = ~7.7ms per ledger in eliminated overhead.

## Anti-Evidence

1. The fused `putFunc` would need to both serialize+hash+write AND accumulate the entry, increasing the lambda's complexity. Error handling would be more complex (partial writes if merge fails).
2. The `BucketOutputIterator` abstraction serves multiple use cases (file-based merges, `fresh()`, `mergeInMemory`). A fused path would bypass this abstraction for a single use case.
3. XDR serialization + SHA256 dominate the output cost (~500ns per entry for serialize + ~200ns for SHA256), so eliminating the ~430ns of redundant copies/comparisons saves ~45% of the non-I/O overhead per entry but only ~25% of total per-entry processing cost.
4. The `std::function` wrapper on `putFunc` already adds ~5-15ns overhead per call; adding serialization inside it doesn't eliminate that overhead.
5. Realistic savings are ~3-8ms per ledger close against 100-500ms total close time = 0.6-8%. Likely at the Low/Informational boundary.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `LiveBucket::mergeInMemory` (LiveBucket.cpp:549-613) and confirmed the
two-phase structure: Phase 1 accumulates entries via `mergeInternal` into a
`mergedEntries` vector through a `putFunc` lambda (lines 584-591), then Phase 2
creates a `BucketOutputIterator` and iterates the entire vector calling `put()`
(lines 599-606). Compared with the file-based `merge()` path
(BucketBase.cpp:392-399), which already does single-pass (putFunc calls
`out.put()` directly during the merge), confirming that the single-pass pattern
is already proven in the codebase. The inefficiency is real but the hypothesis
overstates the savings.

### Code Paths Examined

- `src/bucket/LiveBucket.cpp:mergeInMemory:549-613` — Confirmed two-phase structure: lines 584-591 build `mergedEntries` via `emplace_back`, lines 599-606 iterate through `BucketOutputIterator::put()`. The `BucketOutputIterator` is created only after `mergeInternal` completes.
- `src/bucket/BucketOutputIterator.cpp:put:78-165` — Confirmed buffer-based dedup: `mCmp(*mBuf, e)` key comparison (line 150) and `*mBuf = e` deep copy (line 164) are redundant when input is pre-sorted/deduped from `mergeInternal`. Level-0 always uses `keepTombstoneEntries=true`, so tombstone elision (line 94-98) is never triggered.
- `src/bucket/BucketOutputIterator.cpp:constructor:24-74` — Constructor writes METAENTRY via `put()` (line 55), so the iterator is ready to accept entries immediately after construction; no ordering constraint prevents moving creation before the merge.
- `src/bucket/BucketBase.cpp:merge:341-427` — File-based merge already uses single-pass: `putFunc` at line 397 calls `out.put(entry)` directly. This proves the pattern.
- `src/bucket/BucketBase.cpp:mergeInternal:289-337` — Calls `mergeCasesWithDefaultAcceptance` and `mergeCasesWithEqualKeys` which fully deduplicate; output is sorted and unique-keyed.
- `src/bucket/BucketMergeAdapter.h:MemoryMergeInput:106-175` — Returns `const&` to entries in the original in-memory bucket vectors; entries are hot in cache when `putFunc` is called.
- `src/util/XDRStream.h:writeOne:483-515` — Serializes entry to buffer via `xdr_size` + `xdr_put`, hashes, and writes. This is the dominant cost per entry (~700ns for serialize+hash).

### Findings

**The inefficiency exists but savings are overstated.** The hypothesis claims
eliminating ~7.7ms per ledger, but this conflates two optimization tiers:

**Tier 1 — Simple fusion (call `out.put()` inside `putFunc`):** This is the
safe, clean optimization. It eliminates Phase 2's loop overhead and improves
cache locality (each entry is serialized while still hot from `mergeInternal`).
However, `put()` still performs its key comparison and `*mBuf = e` deep copy.
Realistic savings: **~0.5-1.0ms** per ledger (cache locality only).

**Tier 2 — Bypass `put()` buffer entirely:** Since `mergeInternal` output is
sorted and deduplicated, and level-0 always keeps tombstones, the `put()`
buffer is fully redundant. Bypassing it (writing directly via
`mOut.writeOne()`) eliminates the key comparison (~30ns × 10K = 0.3ms) and
the `*mBuf = e` deep copy (~200ns × 10K = 2.0ms). However, this requires
exposing `BucketOutputIterator` internals or adding a new `putDirect()` method.
Realistic savings: **~2-3ms** per ledger.

**Impact assessment:** Against 100-500ms total ledger close time, Tier 1 saves
0.1-1.0% and Tier 2 saves 0.4-3.0%. Neither reaches the 5% threshold for Low
severity. The hypothesis's ~7.7ms estimate double-counts: the `emplace_back`
deep copy into `mergedEntries` is NOT eliminated (still needed for in-memory
bucket state), and cache miss estimates assume worst-case L2 eviction that may
not occur for 10K × 300-byte entries (~3MB working set) on modern CPUs with
large L2/L3 caches.

**Correctness is confirmed:** The `BucketOutputIterator` constructor writes
METAENTRY immediately, so it can be created before the merge. Level-0 merges
are synchronous on the main thread, so no threading concerns. The
`mergedEntries` vector is still accumulated and moved into the output bucket
via `getBucket()`. Error handling is preserved: if `mergeInternal` throws,
the temp file is cleaned up normally (never adopted).

### PoC Guidance

- **Target code**: `src/bucket/LiveBucket.cpp:mergeInMemory` (lines 549-613)
- **Change description**: Move the `BucketOutputIterator out` construction (lines 599-601) before the `mergeInternal` call (line 590). Change `putFunc` to both accumulate entries AND call `out.put()`:
  ```cpp
  LiveBucketOutputIterator out(bucketManager.getTmpDir(),
                               /*keepTombstoneEntries=*/true, meta, mc, ctx,
                               doFsync);
  std::function<void(BucketEntry const&)> putFunc =
      [&mergedEntries, &out](BucketEntry const& entry) {
          mergedEntries.emplace_back(entry);
          out.put(entry);
      };
  mergeInternal(bucketManager, inputSource, putFunc, maxProtocolVersion, mc,
                shadowIterators, keepShadowedLifecycleEntries);
  // Remove the Phase 2 loop entirely (lines 603-606)
  ```
- **Correctness check**: Existing tests covering level-0 merges: `[bucket]` tag tests, particularly `mergeInMemory` tests. Run `./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[bucket]"`.
- **Benchmark focus**: Run apply-load SAC 3200 T=1 benchmark. Measure median ledger close time. Expected improvement: 0.1-1.0% (Tier 1). If pursuing Tier 2 (bypassing `put()`), add a `putDirect()` method and expect 0.4-3.0%.
