# H001: Bulk prefetch re-reads the same bucket page for adjacent keys

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: disk-read CPU and bulk-prefetch throughput
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

During bulk prefetch, once a bucket index returns a file-page offset for a key, later keys that map to the same page should be satisfied from that already-decoded page until the scan advances past it. A 1000-key prefetch batch should therefore do roughly one page read per touched page, not one page read per key.

## Mechanism

`DiskIndex::scan` returns page-start offsets, but `SearchableBucketListSnapshot::loadKeysFromBucket` immediately calls `getEntryAtOffset` for every `FILE_OFFSET` hit. `getEntryAtOffset` does `stream.seek(pos)` and `readPage(...)` every time, so adjacent keys that share the same 16KB page repeatedly decode the same XDR page and thrash the same stream position instead of amortizing the read across multiple keys.

## Trigger

Run apply-load with default prefetch enabled (`PREFETCH_BATCH_SIZE=1000`) and a ledger whose classic prefetch set contains many nearby account / trustline / liquidity-pool keys in the same bucket page. The issue is strongest when `APPLY_LOAD_LEDGER_MAX_DISK_READ_LEDGER_ENTRIES` is high enough that transactions actually pull many classic entries from buckets.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:getEntryAtOffset:139-164` — seeks and scans a page for each key hit
- `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket:210-277` — bulk loader loops keys one-by-one and calls `getEntryAtOffset` repeatedly
- `src/bucket/DiskIndex.cpp:scan:59-85` — returns page offsets, not entry offsets
- `src/ledger/LedgerTxn.cpp:prefetch:3045-3097` — bulk prefetch path that feeds `loadLiveKeys`

## Evidence

`loadKeysFromBucket` keeps an index iterator across sorted keys, so it already exploits key ordering at the index level. But after every `FILE_OFFSET` result it re-enters `getEntryAtOffset`, which unconditionally seeks to the page offset and runs `readPage` again, even when the previous key resolved from the same page.

## Anti-Evidence

If most keys are cache hits, all buckets are in-memory, or the prefetch set is sparse enough that adjacent keys rarely land in the same page, the gain will be smaller. Bloom-filter negatives also bypass the repeated page-read path.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete bulk-load path from `loadKeysInternal` → `loadKeysFromBucket` → `getEntryAtOffset` → `XDRInputFileStream::readPage`. Confirmed that `DiskIndex::scan()` returns the same `std::streamoff` page-start offset for all keys within a single `RangeEntry`, and `getEntryAtOffset` unconditionally calls `seek(pos)` + `readPage(be, k, pageSize)` for every key. The `XDRInputFileStream` has no page cache — `mBuf` is a reusable buffer, not a cache. Two keys in the same page trigger two full 16KB reads and two separate XDR deserialization passes.

### Code Paths Examined

- `src/bucket/DiskIndex.cpp:scan:59-85` — Returns `keyIter->second` (a `std::streamoff`) from the `RangeIndex`. All keys within a range entry return the identical offset. The iterator is advanced correctly for subsequent keys, but the file offset is not deduplicated.
- `src/bucket/BucketListSnapshot.cpp:getEntryAtOffset:140-164` — Calls `stream.seek(pos)` then `stream.readPage(be, k, pageSize)`. No check for whether the stream is already positioned at the target page.
- `src/util/XDRStream.h:readPage:180-240` — Reads `pageSize` bytes (default 16KB, `2^14`) via `mIn.read(mBuf.data(), pageSize)`, then XDR-deserializes entries one by one until finding the matching key. The buffer `mBuf` is overwritten on each call.
- `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket:210-277` — Iterates sorted keys, calling `scan()` then `getEntryAtOffset()` for each `FILE_OFFSET` result. No tracking of the last page offset to detect same-page keys.
- `src/bucket/DiskIndex.cpp:220-244` — Index construction: entries within the same `pageSize` boundary share a `RangeEntry`, confirming that adjacent keys in the sorted set will produce the same offset from `scan()`.
- `src/main/Config.cpp:176` — Default `BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT = 14` (16KB pages).

### Findings

The inefficiency is confirmed: when two or more sorted keys in a bulk-load batch fall within the same 16KB index page of a bucket, `loadKeysFromBucket` performs redundant `seek()` + `readPage()` calls. Each redundant call re-reads 16KB from the file (served from OS page cache, so no disk I/O but still a memory copy + syscall) and re-deserializes all XDR entries in the page up to the target key.

**Severity downgrade rationale (Medium → Informational):**

1. **Key density per page is workload-dependent.** Keys in a prefetch batch are distributed across up to 22 buckets (11 levels × curr/snap). Within each bucket, the probability of two keys sharing a 16KB page depends on bucket size and key distribution. For large buckets (levels 7–10, where most data resides), 16KB pages contain ~30–80 entries out of millions — key collisions per page are rare.

2. **OS page cache eliminates I/O cost.** The redundant reads hit the kernel page cache, not disk. The cost is a `seekg()` call, a 16KB memcpy, and XDR deserialization — CPU overhead, not I/O.

3. **Existing optimizations reduce exposure.** The in-memory cache (`CACHE_HIT` path) and bloom filter (`NOT_FOUND` path) bypass `getEntryAtOffset` entirely. Only keys that miss the cache and pass the bloom filter reach the redundant read path.

4. **Prefetch is one component of apply time.** Even a significant improvement in bulk-load throughput translates to a modest fraction of total ledger-close time.

The finding is real and the fix is straightforward, but confident prediction of >5% improvement on any benchmark scenario is not supported without measurement.

### PoC Guidance

- **Target code**: `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket` (lines 210–277) and `src/bucket/BucketListSnapshot.cpp:getEntryAtOffset` (lines 140–164).
- **Change description**: In `loadKeysFromBucket`, track the last `std::streamoff` and the last `BucketT const*` used for a `FILE_OFFSET` read. When the next key produces the same offset from the same bucket, instead of calling `getEntryAtOffset` (which seeks and re-reads the page), re-scan the existing `mBuf` in the `XDRInputFileStream` for the new key. This could be implemented either (a) by adding a `readPageCached(key)` method to `XDRInputFileStream` that checks whether the buffer already contains the target page, or (b) by modifying `loadKeysFromBucket` to batch same-page keys and resolve them all from a single `readPage` call. Approach (b) is cleaner since it keeps the caching logic local to the bulk-load path.
- **Correctness check**: Existing tests in `src/bucket/test/BucketIndexTests.cpp` (bulk load tests with `loadKeys` / `loadKeysFromBucket`) cover this path. The `BucketListIsConsistentWithDatabase` and `BucketIndexTests` test suites should pass unchanged.
- **Benchmark focus**: Measure bulk `loadKeys` latency with a batch of 1000+ keys that have moderate page sharing (e.g., many trustlines for the same asset). The metric to watch is total CPU time in `loadKeysFromBucket` and `readPage`, especially page reads per unique page touched. Expect modest improvement (likely < 5% on overall apply-load benchmarks, potentially higher on microbenchmarks of the bulk-load path in isolation).

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-10
**PoC by**: claude-opus-4.6, high

### Changes Made

1. **`src/util/XDRStream.h` (added `scanPage` method, ~45 lines)** — Added `scanPage()` template method to `XDRInputFileStream` that re-scans the existing `mBuf` for a different key without seeking or re-reading from disk. Handles boundary-crossing entries by reading extra bytes from the stream (which is correctly positioned at the byte after `mBuf`'s content — the invariant maintained by `readPage`).

2. **`src/bucket/BucketListSnapshot.h` (added declaration, ~5 lines)** — Declared `getEntryFromExistingPage()` protected method on `SearchableBucketListSnapshot<BucketT>`, parallel to the existing `getEntryAtOffset()`.

3. **`src/bucket/BucketListSnapshot.cpp` (added `getEntryFromExistingPage` + modified `loadKeysFromBucket`, ~35 lines changed)** — Implemented `getEntryFromExistingPage()` which calls `stream.scanPage()` instead of `seek()` + `readPage()`. Modified `loadKeysFromBucket()` to track `lastPageOffset` (`std::streamoff`, initialized to -1). When a `FILE_OFFSET` result matches `lastPageOffset`, the existing page buffer is re-scanned via `getEntryFromExistingPage()` instead of re-reading from disk.

### Demonstration

When multiple sorted keys in a bulk-load batch fall within the same 16KB index page of a bucket, the optimization eliminates redundant `seek()` syscalls, 16KB `read()` memcpy operations, and duplicate XDR deserialization passes. Instead of one full page read per key, same-page keys re-scan the already-loaded in-memory buffer. This reduces CPU overhead on the bulk prefetch path proportional to the page-sharing density of the key set.

### Test Results

- All 13 `[bucketindex]` tests passed (1,088,147 assertions)
- All 47 `[bucket]` tests passed (1,791,020 assertions)
- Full `make check` suite passed (all partitions, including Rust tests)
