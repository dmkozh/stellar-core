# H008: Reusing a canonical meta-only hot-archive bucket is too small to matter

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: hot-archive empty-batch bookkeeping
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When an empty hot-archive batch would serialize the same protocol-versioned
meta-only bucket as previous ledgers, the code could reuse that immutable bucket
directly instead of regenerating the same temporary file. This would preserve
the canonical hot-archive hash while avoiding redundant local serialization.

## Mechanism

`HotArchiveBucket::fresh()` creates a `BucketOutputIterator` even for empty
input, and `BucketOutputIterator`'s constructor immediately buffers a
`HOT_ARCHIVE_METAENTRY`. Because `BucketMetadata` only depends on protocol
version and bucket-list type here, the resulting meta-only bucket is
deterministic during apply-load, so a cached singleton or direct hash lookup
seems viable in principle.

## Trigger

Run apply-load with the stock large-TTL settings so archival stays inactive and
hot-archive batches are empty on nearly every ledger.

## Target Code

- `src/bucket/HotArchiveBucket.cpp:fresh:16-44` — always instantiates an output iterator even for empty input
- `src/bucket/BucketOutputIterator.cpp:25-73` — buffers the metadata entry before any archived/restored payload exists
- `src/bucket/BucketOutputIterator.cpp:getBucket:169-249` — hashes and adopts the meta-only temp file

## Evidence

The empty-input hot-archive path is deterministic and file-backed today: the
iterator seeds the metadata entry, `getBucket()` flushes it, computes a stable
hash, and adoption deduplicates against any existing bucket with the same hash.
That means the steady-state remaining work is just "serialize tiny bucket to a
temp file, hash it, then delete it because the canonical bucket already exists."

## Anti-Evidence

Hash deduplication and merge reattachment already eliminate the expensive parts
of the pipeline after the first few ledgers. What remains is only a tiny
temp-file roundtrip over a few dozen bytes.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

Even the safer "reuse the same meta-only bucket" variant only removes the tiny
steady-state temp-file serialize/hash/delete step that remains after bucket-hash
deduplication. That overhead is far below the threshold for a meaningful
apply-load improvement.

### Lesson Learned

When a path already deduplicates downstream work by hash, eliminating the final
front-end serialization only helps if the serialized payload is still large
enough to matter. For meta-only hot-archive buckets, the remaining work is too
small to prioritize.
