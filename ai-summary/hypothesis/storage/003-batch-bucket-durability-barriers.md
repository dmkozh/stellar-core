# H003: Batch bucket durability barriers at snapshot commit instead of per file

**Date**: 2026-04-10
**Subsystem**: storage (bucket, ledger)
**Severity**: Medium
**Impact**: write-path latency reduction by coalescing bucket/index fsync and directory-fsync barriers
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

All bucket and bucket-index files that become reachable from the persisted
`bucketListHash` should be durable before the ledger header is stored, but the
system should not need to pay a separate file-fsync and directory-fsync barrier
for every intermediate bucket artifact earlier in the close. Durability should
be enforced at the point where the ledger snapshot becomes externally committed,
not repeatedly during intermediate bucket production.

## Mechanism

Today every bucket write closes its temp file through `OutputFileStream::close()`,
which fsyncs the file, and every adoption uses `durableRename(...)`, which fsyncs
the bucket directory. This happens for synchronous level-0 output every ledger,
for background level-1+ merges, and again for persisted `.index` files. But the
new bucket hashes are not incorporated into the ledger header until later, when
`snapshotLedger(lh)` runs just before the header is stored.

That suggests the barriers are earlier and more numerous than necessary. If
adopted bucket/index files were tracked as "pending durability" and flushed once
per ledger (or once per level commit before a future-backed bucket is allowed to
participate in `bucketListHash`), the system could preserve crash safety while
removing multiple serialized fsync points from the apply path. This should be
most visible in T=8 runs where the serial write tail dominates after parallel tx
execution finishes.

## Trigger

Run apply-load with `APPLY_LOAD_TIME_WRITES = true`. Every ledger performs a
level-0 bucket write on the critical path, and spill ledgers add background
merge outputs and possibly persisted index files, each currently carrying its own
close+fsync and durable-rename sequence.

## Target Code

- `src/bucket/BucketListBase.cpp:addBatchInternal:777-783` — level 0 always writes with `doFsync = !DISABLE_XDR_FSYNC`
- `src/bucket/FutureBucket.cpp:406-427` — background merges also inherit `doFsync`
- `src/bucket/BucketOutputIterator.cpp:getBucket:181-235` — closes the file before adoption, triggering file fsync
- `src/util/XDRStream.h:307-320` — `OutputFileStream::close()` flushes and fsyncs on close
- `src/bucket/BucketManager.cpp:430-442` — `renameBucketDirFile()` uses `durableRename`, which fsyncs the directory
- `src/bucket/DiskIndex.cpp:349-371` — persisted index files pay the same close+rename durability sequence
- `src/ledger/LedgerManagerImpl.cpp:3053-3056` — bucket writes finish before in-memory-state update
- `src/ledger/LedgerManagerImpl.cpp:3102-3108` and `src/bucket/BucketManager.cpp:1106-1134` — `snapshotLedger()` and header persistence happen later

## Evidence

The code clearly separates "produce/adopt bucket files" from "commit the ledger
header that references their hash." That gap means the current durability model
is synchronizing each artifact individually rather than synchronizing the ledger
snapshot as a unit. Because apply-load explicitly times writes, and level-0
bucket production is on the serial close path every ledger, redundant fsync and
directory-fsync barriers are a plausible multi-millisecond source of avoidable
latency.

## Anti-Evidence

The crash-consistency protocol is subtle: background merge outputs can live for
many ledgers before they are promoted, and the code currently relies on bucket
adoption producing fully durable files immediately. Any batching scheme would
need a careful rule for when a bucket may become hash-visible and when pending
durability must be forced, so the implementation risk is materially higher than
the more local storage optimizations.
