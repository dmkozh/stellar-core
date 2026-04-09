# H001: Level-0 live buckets are still materialized to disk on every ledger

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: High
**Impact**: ledger-close serial CPU/I/O
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When level 0 is merged entirely in memory, the resulting bucket should stay in memory until a later spill or publication step actually requires a file-backed artifact. The close path should not synchronously serialize, fsync, rename, and reopen a level-0 bucket every ledger if the same merged entries are already resident in `mEntries` and only their hash/index are needed for the next ledger.

## Mechanism

`BucketLevel<LiveBucket>::prepareFirstLevel` correctly avoids file I/O for the incoming "level -1" snap by using `freshInMemoryOnly`, but `LiveBucket::mergeInMemory` immediately turns the merged result back into a real bucket file. That reintroduces a full serialize/hash/write/adopt cycle onto the main thread every ledger even though the returned `LiveBucket` still retains the merged entries in memory, so the close path pays both the in-memory merge cost and a second file-materialization cost for the same data.

## Trigger

Run any write-heavy apply-load benchmark (`sac`, `custom_token`, or `soroswap`) with `APPLY_LOAD_TIME_WRITES=true`. The issue is strongest once each ledger modifies thousands of live entries, because level 0 is rebuilt on every close regardless of whether it will spill.

## Target Code

- `src/bucket/BucketListBase.cpp:196-238` — level-0 fast path uses `freshInMemoryOnly`, then still calls `mergeInMemory`
- `src/bucket/LiveBucket.cpp:550-612` — `mergeInMemory` builds `mergedEntries` in RAM and then serializes them again through `BucketOutputIterator`
- `src/bucket/BucketManager.cpp:1106-1134` — `snapshotLedger` only consumes bucket hashes, not bucket files
- `src/bucket/BucketBase.cpp:60-110` — non-empty buckets are modeled as file-backed (`filename` + `hash`), forcing materialization today

## Evidence

The merged level-0 result is already available as `mergedEntries` before any file I/O begins, and `out.getBucket(..., inMemoryState=...)` stores the same vector in the returned `LiveBucket` for the next ledger's in-memory merge. That means the close path is doing a full extra pass just to create a durable bucket artifact for a bucket that is immediately reused from memory on the next close.

## Anti-Evidence

The current bucket abstraction assumes non-empty buckets have filenames, and restart/publication logic expects file-backed buckets. A real fix therefore likely needs a new "hashed but not yet materialized" bucket state or a targeted level-0-only lazy materialization path rather than a trivial early return.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full level-0 merge path from `addBatchInternal` through `prepareFirstLevel` → `mergeInMemory` → `BucketOutputIterator` → `adoptFileAsBucket`. Confirmed that on every ledger, the in-memory merge produces a `mergedEntries` vector, then does a second full pass to XDR-serialize every entry, compute SHA256, and write to disk. The resulting bucket stores both the file and the in-memory entries, but only the in-memory entries are consumed on the subsequent ledger: the `InMemoryIndex` serves all BucketListDB queries via CACHE_HIT (no file reads), and the next `mergeInMemory` reads from `getInMemoryEntries()`. The file is only needed when level 0 spills (every 2 ledgers), at which point `BucketInputIterator` reads from it for the level-1 merge.

### Code Paths Examined

- `src/bucket/BucketListBase.cpp:196-238` (`prepareFirstLevel`) — On every ledger, creates snap via `freshInMemoryOnly` (no disk), then calls `mergeInMemory` which DOES write to disk. On even ledgers (after snap resets mCurr to empty), the empty bucket's constructor sets `mEntries` to an empty vector, so `hasInMemoryEntries()` returns true and the in-memory path is still taken.
- `src/bucket/LiveBucket.cpp:549-613` (`mergeInMemory`) — Merges entries in-memory into `mergedEntries` (fast), then creates `BucketOutputIterator` (opens temp file), writes all entries via `out.put(e)` which XDR-serializes + SHA256-hashes + writes to disk per entry, then `out.getBucket()` finalizes file, creates `InMemoryIndex` from the in-memory state, calls `adoptFileAsBucket` which renames temp to canonical path.
- `src/bucket/BucketOutputIterator.cpp:25-74,76-165,167-250` — Constructor opens file and writes METAENTRY. `put()` buffers one entry for dedup, writes to `mOut` (XDROutputFileStream) with SHA256 hashing. `getBucket()` closes file, computes final hash, constructs index (from in-memory state if available, bypassing file read), calls `adoptFileAsBucket`.
- `src/bucket/LiveBucketIndex.cpp:84-92,224-240` — When constructed from `inMemoryState`, creates `InMemoryIndex` (no DiskIndex). `lookup()` delegates to `InMemoryBucketState::scan()` which returns CACHE_HIT with the entry from the in-memory hash set — no file access needed.
- `src/bucket/InMemoryIndex.cpp:64-76` (`InMemoryBucketState::scan`) — Finds key in `unordered_set`, returns `IndexReturnT(it->get())` which is CACHE_HIT. Queries never reach the file.
- `src/bucket/BucketListSnapshot.cpp:171-201` (`getBucketEntry`) — For CACHE_HIT, returns immediately from memory. FILE_OFFSET case (only for DiskIndex buckets) would read from file — never triggered for level-0 InMemoryIndex buckets.
- `src/bucket/BucketListBase.cpp:540-551` (`levelShouldSpill`) — Level 0 spills at every even ledger (levelHalf(0) = 2). File is consumed by level 1's `FutureBucket` merge via `BucketInputIterator`.
- `src/bucket/BucketListSnapshot.cpp:654-684` (`scanForEntriesOfType`) — Uses file seek + read via `loopAllBuckets`, including level 0. This is an additional file consumer beyond spill, though rarely called on the hot path.
- `src/bucket/BucketBase.cpp:60-65` — `releaseAssert(filename.empty() || fs::exists(filename))` enforces that non-empty buckets must have files on disk.

### Findings

**The inefficiency is real.** On every ledger close, `mergeInMemory` performs a complete XDR-serialize + SHA256 + disk-write pass over all level-0 entries, producing a file that is not accessed until level 0 spills (every 2 ledgers). Between spills, the file sits unused — queries are served from the `InMemoryIndex` and the next merge reads from `mEntries`.

**The savings are narrower than claimed.** The hypothesis claims High severity, but:
1. **XDR serialization + SHA256 cannot be eliminated** — the bucket hash is required for `snapshotLedger()` to compute `LedgerHeader.bucketListHash` (consensus-critical). The hash is computed over serialized XDR bytes, so serialization must occur. The savings is limited to avoiding the physical disk I/O (write syscalls, fsync, file rename).
2. **Level 0 spills every 2 ledgers**, so the file is needed very soon. The optimization defers writes, saving ~50% of file materializations (the odd-ledger ones). On spill ledgers, the file must exist before `snap()`.
3. **Level 0 buckets are small** relative to deeper levels (holding ~2 ledgers of entries), limiting absolute I/O savings.
4. **Additional consumers exist** — `scanForEntriesOfType` reads from level 0 files (though infrequently). An unmaterialized bucket would need an alternative code path here.
5. **Architectural constraints are significant** — `BucketBase` asserts files exist for non-empty buckets (`BucketBase.cpp:65`), `BucketManager` maps use hash→bucket with filename, and restart/publication logic expects file-backed buckets.

**A more impactful variant** would compute the hash during the merge in a single pass (serialize + hash each entry inline in the `putFunc` lambda instead of emitting to `mergedEntries` and then re-serializing through `BucketOutputIterator`). This eliminates the second full pass over the data. The `BucketOutputIterator` dedup logic (adjacent same-key elision) would need to be replicated, but for level 0 merges this is straightforward since `mergeInternal` already handles key conflicts.

**Severity assessment**: The physical disk I/O savings (write + fsync + rename) per ledger is likely 1-10ms for level-0-sized buckets. Against total ledger close times of 100-500ms (dominated by transaction execution), this represents roughly 1-5%. Below the 5-10% threshold for Low, but the combined single-pass optimization (avoiding the second serialization pass entirely) could push closer to 5% for write-heavy workloads.

### PoC Guidance

- **Target code**: `src/bucket/LiveBucket.cpp:549-613` (`mergeInMemory`) and `src/bucket/BucketOutputIterator.cpp` (the `put` + `getBucket` flow)
- **Change description**: Modify `mergeInMemory` to compute the bucket hash inline during the merge (add XDR serialization + SHA256 update to the `putFunc` lambda), avoiding the second pass through `BucketOutputIterator`. Defer file materialization to spill time by introducing a `materializeToFile()` method on `LiveBucket` that writes in-memory entries to disk. Relax the `BucketBase` filename assertion for level-0 in-memory buckets. Alternatively, as a simpler first step, write to a memory buffer instead of a file in `BucketOutputIterator` and only flush to disk at spill time.
- **Correctness check**: Existing tests covering level-0 merges include `[bucket]` tagged tests — run `"[bucket]"` test suite. Key tests: BucketList merge tests, BucketListDB lookup tests, restart/merge-reattachment tests. Verify `bucketListHash` consensus matches by running multi-node tests.
- **Benchmark focus**: Measure `bucket.merge.level-0.time` (if instrumented) or total ledger close time on `sac` and `soroswap` benchmarks at T=8. The metric to watch is per-ledger close latency reduction. Expect 1-5% improvement on close time for write-heavy workloads; improvement scales with number of state changes per ledger.
