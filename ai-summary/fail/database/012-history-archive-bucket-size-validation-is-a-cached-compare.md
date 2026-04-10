# H012: HistoryArchiveState Revalidates Bucket Sizes Expensively on Every Ledger Close

**Date**: 2026-04-10
**Subsystem**: database, history
**Severity**: Informational
**Impact**: CPU overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If bucket-size validation during `HistoryArchiveState` construction were
expensive, the ledger-close path should avoid repeating it every ledger and
instead validate only when buckets are created or published. The close path
should not hit filesystem metadata or otherwise costly size computation for each
bucket level.

## Mechanism

`HistoryArchiveState` checks `curr->getSize()` and `snap->getSize()` for every
live-bucket level while constructing the object used for DB persistence. Since
this happens on every ledger close, it initially looks like a repeated
filesystem-size or metadata-read path hidden inside restart-state persistence.

## Trigger

Run any apply-load scenario and inspect `HistoryArchiveState` construction in
the per-ledger persistence helper.

## Target Code

- `src/history/HistoryArchive.cpp:HistoryArchiveState ctor:537-564` — per-level `checkBucketSize()` calls on every ledger close
- `src/bucket/BucketBase.h:getSize:106-109` — public size accessor
- `src/bucket/BucketBase.cpp:BucketBase ctor/getSize:60-71,92-97` — bucket size is cached in `mSize` at construction and `getSize()` just returns that field

## Evidence

The `HistoryArchiveState` constructor does iterate all levels and calls
`bucket->getSize()` twice per level, which is an obvious place to look for
hidden I/O.

## Anti-Evidence

`BucketBase` caches the filesystem size once in its constructor, and
`getSize()` is just a field read. The per-ledger work here is therefore only a
small fixed number of integer comparisons.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The suspected expensive path is not expensive: `bucket->getSize()` does not hit
the filesystem during ledger close, it returns the cached `mSize` field.

### Lesson Learned

When a hot-path constructor calls accessors that look I/O-shaped, trace the
accessor implementation before assuming runtime filesystem cost. In this case
the expensive-looking check collapsed to a handful of cached integer reads.
