# H001: Level-0 BucketList `mergeInMemory` Performs Synchronous Disk Write + Fsync on Critical Path

**Date**: 2026-04-10
**Subsystem**: ledger (LiveBucket, BucketOutputIterator, BucketListBase)
**Severity**: High
**Impact**: Eliminate 6-25ms of synchronous I/O per ledger close across all Soroban scenarios
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The level-0 BucketList merge (`mergeInMemory`) should compute the bucket
hash synchronously (needed for the ledger header's `bucketListHash`) but
defer the file write and fsync to a background thread. Since the merged
bucket already retains all entries in memory (via
`std::make_unique<std::vector<BucketEntry>>(std::move(mergedEntries))`),
the in-memory copy is sufficient for the next level-0 merge. The on-disk
file is only needed when a higher-level merge reads it or when the bucket
is published to a history archive — both of which happen asynchronously
and have multiple ledgers of slack time before they're needed.

## Mechanism

`LiveBucket::mergeInMemory` (LiveBucket.cpp:549-613) is called on every
ledger close from `BucketLevel<LiveBucket>::prepareFirstLevel`
(BucketListBase.cpp:235-237). After performing an in-memory merge of the
old level-0 curr bucket with the new entries, it writes ALL merged entries
to disk via `LiveBucketOutputIterator`:

```
LiveBucketOutputIterator out(bucketManager.getTmpDir(),
                             /*keepTombstoneEntries=*/true, meta, mc, ctx,
                             doFsync);                          // line 599
for (auto const& e : mergedEntries)
{
    out.put(e);                                                 // line 605
}
return out.getBucket(bucketManager, nullptr,
    std::make_unique<std::vector<BucketEntry>>(std::move(mergedEntries)));
```

Each `out.put(e)` call (BucketOutputIterator.cpp:78-165) XDR-serializes
the entry to a buffer, writes the buffer to an ASIO buffered file stream,
and feeds the buffer to a SHA-256 hasher. On `getBucket()`, the stream is
closed and fsynced (when `doFsync=true`, which is the default — the
benchmark does NOT set `DISABLE_XDR_FSYNC`).

For the SAC benchmark with TX=3200: each ledger produces ~6K-12K new
entries. The existing level-0 curr has entries from the previous ledger(s).
The merged output has ~12K-24K entries × ~200-500 bytes each = ~4-12MB
of data. The costs on the critical path:

1. **XDR serialization**: ~4-12MB serialized — ~2-5ms (needed for hash)
2. **SHA-256 hashing**: ~4-12MB hashed — ~2-4ms (needed for hash)
3. **Buffered file write**: ~4-12MB written — ~1-3ms
4. **Fsync**: forces pages to SSD — ~5-15ms

Items 3-4 (total ~6-18ms) are pure I/O that could be deferred to a
background thread. The XDR serialization and hash computation (items 1-2)
must remain synchronous because `snapshotLedger` (called immediately after
`addLiveBatch`) reads the bucket hash for the ledger header.

The on-disk file is not needed until either (a) a level-1 merge reads this
bucket as an input, or (b) the bucket is referenced in a history archive
checkpoint. Level-1 spills happen every 8 ledgers, giving 4+ ledgers of
slack. History publication is fully asynchronous. With 50-200ms per ledger,
there is 200-1400ms of wall time to complete the async write — far more
than the ~10-30ms the write actually takes.

## Trigger

Run `scripts/run_apply_load_matrix.py` for any Soroban scenario (sac,
custom_token, soroswap). Profile the `LiveBucket::mergeInMemory` Tracy
zone. The synchronous I/O time within `BucketOutputIterator::put()` and
`getBucket()` (fsync) should be visible as a significant fraction of the
`sealLedgerTxnAndStoreInBucketsAndDB` phase.

## Target Code

- `src/bucket/LiveBucket.cpp:mergeInMemory:549-613` — creates `LiveBucketOutputIterator` and writes all merged entries to disk on the critical path
- `src/bucket/LiveBucket.cpp:mergeInMemory:599-606` — the `LiveBucketOutputIterator` construction and `put()` loop that performs synchronous I/O
- `src/bucket/BucketOutputIterator.cpp:put:78-165` — XDR-serializes each entry, writes to buffered stream, updates hash
- `src/util/XDRStream.h:writeOne:483-515` — serializes to buffer, calls `writeBytes`, then `hasher->add`
- `src/util/XDRStream.h:writeBytes:408-448` — ASIO buffered write to file descriptor
- `src/bucket/BucketOutputIterator.cpp:getBucket` — closes stream (triggers flush + fsync)
- `src/bucket/BucketListBase.cpp:prepareFirstLevel:196-238` — calls `mergeInMemory` and `commit()` for level-0

## Evidence

1. `mergeInMemory` creates a `LiveBucketOutputIterator` with `doFsync` parameter (line 600), and the benchmark uses the default `DISABLE_XDR_FSYNC=false` — meaning fsync is ENABLED in the benchmark.
2. The merged entries are ALREADY kept in memory at line 612: `std::make_unique<std::vector<BucketEntry>>(std::move(mergedEntries))`. The in-memory copy is used for subsequent level-0 merges via `hasInMemoryEntries()` / `getInMemoryEntries()`.
3. `freshInMemoryOnly` (line 467-498) already demonstrates that level-0 buckets CAN skip disk write: it creates a "shell" bucket with no file, just in-memory entries. The asymmetry — `freshInMemoryOnly` skips I/O but `mergeInMemory` doesn't — suggests the disk write in `mergeInMemory` was not optimized for the in-memory-merge path.
4. Level-1 merges happen every 8 ledgers (BucketListBase.cpp:526), giving 4+ ledgers of slack time for an async disk write to complete before the file is read.
5. The `XDROutputFileStream::writeBytes` (XDRStream.h:408-448) writes via ASIO buffered stream to a file — a synchronous system call path.
6. For T=8 scenarios, the bucket merge runs on the single apply thread AFTER parallel Soroban execution completes. Any I/O here directly limits the maximum speedup from parallelization (Amdahl's law on the serial portion).

## Anti-Evidence

1. The disk write provides crash-recovery durability — if stellar-core crashes after computing the hash but before the async write completes, the bucket file would be missing. However, the BucketList can be reconstructed from the database via catchup, and the HAS in SQL provides the authoritative state.
2. Separating hash computation from disk write requires refactoring `BucketOutputIterator` or creating a new hash-only output path, which is a non-trivial code change.
3. The buffered write path (ASIO buffered stream) may already batch most I/O into few syscalls, reducing the per-entry overhead. The dominant cost may be the final fsync rather than the writes themselves.
4. If the SSD subsystem is fast enough (NVMe with <1ms fsync), the total I/O cost may be only 3-5ms rather than the estimated 6-18ms, reducing the optimization to Low severity.
5. The benchmark environment's I/O characteristics may differ from production — some CI runners use ramdisk or tmpfs, which would make this optimization less impactful in benchmark measurements.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete execution path from `BucketListBase::addBatch` → `BucketLevel::prepareFirstLevel` → `LiveBucket::mergeInMemory` → `BucketOutputIterator::put()` → `XDROutputFileStream::writeOne` → `OutputFileStream::writeBytes` → `asio::write` (synchronous), and the close path through `OutputFileStream::close()` → `flush()` → `fs::flushFileChanges()` → `fsync()`. Confirmed that all file I/O is synchronous and on the critical path. Also traced the spill path that consumes the bucket file: `addBatch` → `BucketLevel::snap()` → `BucketLevel::prepare()` → `FutureBucket::startMerge()` → background thread → `BucketInputIterator` (reads file).

### Code Paths Examined

- `src/bucket/LiveBucket.cpp:549-613` — `mergeInMemory`: confirmed it creates `LiveBucketOutputIterator` and writes all entries synchronously, then stores in-memory entries in the returned bucket
- `src/bucket/LiveBucket.cpp:466-498` — `freshInMemoryOnly`: confirmed it skips all disk I/O, creating only in-memory shell bucket. This proves the in-memory-only path is architecturally sound.
- `src/bucket/BucketOutputIterator.cpp:76-165` — `put()`: each call XDR-serializes, writes to buffered file stream, and hashes. The buffer check at line 148-155 deduplicates consecutive same-key entries.
- `src/util/XDRStream.h:483-515` — `writeOne`: serializes to `mBuf`, calls `writeBytes(mBuf.data(), toWrite)` THEN `hasher->add(ByteSlice(mBuf.data(), toWrite))`. Both operate on the same serialized buffer. The file write could be removed without affecting hashing.
- `src/util/XDRStream.h:407-448` — `writeBytes`: synchronous `asio::write` to buffered write stream (256KB buffer per `fs::bufsz()` = 0x40000). For 4-12MB of data, this triggers ~16-48 actual write syscalls as the buffer fills.
- `src/util/XDRStream.h:307-327` — `close()`: calls `flush()` then conditionally `fs::flushFileChanges()` (fsync). The fsync is the single most expensive I/O operation.
- `src/util/Fs.cpp:224-236` — `flushFileChanges`: calls `fsync(fd)` which forces all dirty pages to persistent storage.
- `src/bucket/BucketOutputIterator.cpp:167-250` — `getBucket()`: writes last buffered entry, closes stream (triggering flush+fsync), computes hash, builds index from in-memory entries (line 226-228), adopts file.
- `src/bucket/BucketListBase.cpp:520-551` — `levelShouldSpill`: confirmed level-0 spills every 2 ledgers (not 8 as hypothesis implies for the relevant slack window).
- `src/bucket/BucketListBase.cpp:728-783` — `addBatch`: on spill ledgers, the loop at line 740 processes spills BEFORE `prepareFirstLevel` at line 781. Level-0's curr (the previous mergeInMemory output) becomes snap and is passed to a background merge.
- `src/bucket/FutureBucket.cpp:347-461` — `startMerge`: posts merge task to background thread. The task calls `BucketT::merge()` which creates `BucketInputIterator` that opens the bucket FILE for reading.
- `src/bucket/BucketInputIterator.cpp:128-146` — constructor: opens the bucket file via `mIn.open(mBucket->getFilename())`. If the file doesn't exist, this would fail.
- `src/bucket/BucketManager.cpp:447-561` — `adoptFileAsBucket`: renames temp file to canonical bucket path. This is the file that `BucketInputIterator` will later open.

### Findings

**The inefficiency is real:** `mergeInMemory` performs synchronous file writes (~16-48 buffered write syscalls for 4-12MB) and an fsync on the critical ledger-close path. The in-memory entries are already retained and used for subsequent level-0 merges. The file write + fsync could theoretically be deferred.

**The slack time estimate is incorrect:** The hypothesis claims "Level-1 spills happen every 8 ledgers, giving 4+ ledgers of slack." This confuses level-1 spills with level-0 spills. Level-0 spills every 2 ledgers (`levelShouldSpill(_, 0)`: 2, 4, 6, ...). When level-0 spills on ledger N, the OLD level-0 curr (produced on ledger N-1 or N-2) is passed to a background level-1 merge that needs the file. The actual slack is **1 ledger** — the bucket file must exist by the next even-numbered ledger's `addBatch` call. At 50-200ms per ledger close, this gives 50-200ms of wall time, which is still sufficient for a deferred 3-15ms write but with much tighter margins than claimed.

**The implementation is more complex than suggested:** Beyond creating a hash-only output path:
1. `BucketOutputIterator::getBucket` calls `adoptFileAsBucket` which renames the temp file to the canonical path. With deferred writes, the file doesn't exist yet at `getBucket` time, requiring a different bucket creation path.
2. The background level-1 merge (`FutureBucket::startMerge` → `BucketInputIterator`) opens the bucket file for reading. A synchronization mechanism is needed to ensure the deferred write completes before any reader opens the file.
3. The `LiveBucketIndex` can be constructed from in-memory entries (line 226-228 of `getBucket`), so index creation is not blocked.

**I/O cost estimate:** The ASIO buffered write stream has a 256KB buffer (`fs::bufsz() = 0x40000`). For 4-12MB of merged output, this means ~16-48 `asio::write` syscalls during the put loop (each flushing a full 256KB buffer), plus a final flush and fsync on close. On NVMe, writes to page cache are fast (~1-3ms total), and fsync is ~1-5ms. On SATA SSD, fsync can be 5-15ms. Total I/O savings: ~2-8ms (NVMe) to ~6-18ms (SATA SSD).

**Severity downgrade rationale:** The hypothesis claims High (>20% improvement), but the analysis suggests the savings are 2-8ms per ledger on NVMe (the likely benchmark hardware). With total ledger close times of 50-200ms, this represents 1-16% improvement. For T=8 SAC TX=3200 (where the serial portion matters most), the improvement is concentrated in the serial tail and could approach 5-10% of total close time. This maps to **Low** severity (5-10% improvement range).

### PoC Guidance

- **Target code**: `src/bucket/LiveBucket.cpp:mergeInMemory` (lines 598-612), `src/bucket/BucketOutputIterator.cpp`, `src/util/XDRStream.h` (OutputFileStream/XDROutputFileStream classes)
- **Change description**: Create a "hash-only" variant of the output path in `mergeInMemory` that serializes entries and computes the SHA-256 hash without writing to disk. Buffer the serialized bytes in memory. After hash computation, post the buffered data + target filename to a background thread for writing + fsync. The bucket object should be created with the hash and in-memory entries immediately; the file path should be set once the background write completes (or the bucket should track a "pending write" future). `BucketInputIterator` should be modified to wait for any pending write before opening the file, or the spill path should explicitly ensure the write is complete before starting the background merge.
- **Correctness check**: Run `[bucket]` tests and the full test suite. Key tests to watch: BucketList merge tests, level-0/level-1 spill tests, crash recovery tests, history publish tests. The `ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING` and `DISABLE_XDR_FSYNC` config options should be considered.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` for SAC TX=3200 T=8 and compare the `sealLedgerTxnAndStoreInBucketsAndDB` Tracy zone duration. Expected improvement: 2-8ms per ledger on NVMe (~3-10% of the seal phase). Also compare the overall ledger close p50/p99 for a ~2-8% improvement.
