# H003: Parallel apply reopens and re-decodes the same bucket files independently in every worker thread

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: parallel apply scalability
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Immutable bucket files should be read through shared backing objects in parallel apply, so eight worker threads touching the same buckets do not each pay their own `open`/`seek`/buffer-fill/XDR-page-decode overhead. Thread-local read position is necessary, but immutable bucket contents should still be shareable through `pread`, mmap, or page objects keyed by `(bucket, offset)`.

## Mechanism

`ThreadParallelApplyLedgerState` copy-constructs a fresh `ApplyLedgerStateSnapshot` per worker, and `SearchableBucketListSnapshot` intentionally resets `mStreams` on copy. As a result, the first lookup against a given bucket in each worker thread lazily opens a new `XDRInputFileStream`, and later page loads decode into that stream's private `mBuf`, so the same bucket page can be reopened and re-decoded independently by multiple threads in the same ledger.

## Trigger

Run the `sac`, `custom_token`, or `soroswap` benchmark at `T=8` with enough clusters that multiple workers consult the same mid/old-level buckets during validation or classic-entry loads. The effect is strongest once the working set no longer fits in level 0 and threads repeatedly hit the same shared bucket pages.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:84-133` — snapshot copies reset `mStreams`; `getStream` lazily opens a file per snapshot copy
- `src/bucket/BucketListSnapshot.cpp:140-164` — page loads operate on the per-snapshot stream object
- `src/util/XDRStream.h:179-240` — `readPage` uses the stream's private mutable buffer and re-decodes the page contents
- `src/transactions/ParallelApplyUtils.cpp:610-623` — each worker thread copies `mLCLSnapshot` into its own thread state

## Evidence

The concurrency fix in the existing code is isolation by copy, not sharing: every copied snapshot gets fresh file caches, and every cache miss calls `stream->open(bucket->getFilename())`. That avoids races, but it also means T=8 can multiply the same bucket-file setup and page-decode work across workers even though the underlying bucket data is immutable.

## Anti-Evidence

The OS page cache will usually prevent repeated physical disk reads, so the win is mostly in syscalls, userspace copies, and duplicated XDR decoding rather than raw storage bandwidth. The fix also has to preserve thread safety, so a shared scheme likely needs `pread`/mmap-style reads rather than simply sharing the current seek-based stream objects.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (distinct from fail/001 which checked contention, not duplication cost)
**Failed At**: reviewer

### Trace Summary

Traced the full parallel apply path from `ThreadParallelApplyLedgerState` construction through `getLiveEntryOpt` to bucket list point lookups. Confirmed that each thread gets its own `SearchableBucketListSnapshot` with independent `mStreams`, and classic entry lookups (ACCOUNT, TRUSTLINE) that aren't in `mGlobalEntryMap` do fall through to `mLCLSnapshot.loadLiveEntry()` → bucket file I/O. However, the actual volume of cross-thread bucket I/O during parallel apply is far too small to produce a measurable benchmark improvement.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:624-639` — `ThreadParallelApplyLedgerState` constructor copy-constructs `mLCLSnapshot` from global, giving each thread its own snapshot with fresh empty `mStreams`.
- `src/transactions/ParallelApplyUtils.cpp:715-750` — `getLiveEntryOpt` checks `mThreadEntryMap` first, then `InMemorySorobanState` for Soroban types (CONTRACT_DATA, CONTRACT_CODE, TTL), and only falls through to `mLCLSnapshot.loadLiveEntry(key)` for non-Soroban classic entries not in the thread map.
- `src/transactions/ParallelApplyUtils.cpp:339-401` — `preParallelApplyAndCollectModifiedClassicEntries` loads classic entries from Soroban footprints into `mGlobalEntryMap`, but ONLY if they exist in the LedgerTxn chain (already modified in this ledger). Uses `getNewestVersionBelowRoot` which returns `{false, nullptr}` at the root (line 3676-3678) — does NOT load from the BucketList.
- `src/ledger/InMemorySorobanState.cpp:145-149` — `isInMemoryType` returns true for CONTRACT_DATA, CONTRACT_CODE, TTL. ACCOUNT and TRUSTLINE are NOT in-memory types.
- `src/bucket/LiveBucketIndex.cpp:334-337` — `isCachedType` returns true only for ACCOUNT. TRUSTLINE lookups always go through bloom filter → FILE_OFFSET → readPage.
- `src/bucket/BucketListSnapshot.cpp:84-96` — Copy constructor confirms `mStreams` left empty; each copy lazily opens file descriptors.
- `src/bucket/BucketListSnapshot.cpp:170-201` — `getBucketEntry` point lookup: CACHE_HIT (ACCOUNT only), FILE_OFFSET (seek + readPage for 16KB), or NOT_FOUND (bloom filter negative).

### Why It Failed

The inefficiency described (duplicated file opens and page decodes across threads) is real but not in a hot enough path to produce a measurable improvement:

1. **Two of three benchmark scenarios have zero bucket I/O during parallel apply.** `custom_token` and `soroswap` footprints contain only Soroban entry types (CONTRACT_DATA, CONTRACT_CODE, TTL), which are served entirely by `InMemorySorobanState` — no bucket list access at all.

2. **For `sac`, only unmodified classic entries (trustlines) hit bucket I/O.** Source/fee accounts are pre-loaded into `mGlobalEntryMap` by `preParallelApplyAndCollectModifiedClassicEntries` (because `preParallelApply` modifies them). Only read-only trustlines in the footprint fall through to the bucket list.

3. **Cross-thread page collisions are extremely unlikely.** Different threads process different clusters with different transactions accessing different trustlines. The probability of two threads hitting the same 16KB page in the same bucket is `O(pages_per_thread² / total_pages)` — negligible for large buckets.

4. **XDR decoding cannot be shared.** Even with mmap or pread, each thread must independently deserialize the XDR page to produce its own `BucketEntry` object. This is the dominant CPU cost per lookup.

5. **Per-lookup overhead is already small.** The OS page cache eliminates disk I/O. The cost per point lookup is: one bloom-filter check per bucket (fast, ~20 buckets say NOT_FOUND), one `lseek` + `read(16KB)` from kernel page cache, one XDR decode pass. Total per-lookup is ~10–50μs.

6. **The implementation complexity is high.** Switching to mmap or pread requires refactoring XDRInputFileStream, adding synchronization for shared page caches, or changing the entire I/O model — all for a sub-millisecond improvement.

### Lesson Learned

Before proposing cross-thread sharing optimizations for bucket I/O during parallel apply, check which entry types actually reach the bucket list. The `InMemorySorobanState` layer (covering CONTRACT_DATA, CONTRACT_CODE, TTL) and the `mGlobalEntryMap` pre-loading of modified classic entries together eliminate nearly all bucket I/O for Soroban-dominated workloads. Only non-cached, non-pre-loaded classic entries (primarily read-only trustlines in SAC scenarios) hit the bucket file path, and the volume is too low for cross-thread sharing to be worthwhile.
