# H004: xdr_size + xdr_put Double Traversal in BucketOutputIterator writeOne

**Date**: 2025-07-22
**Subsystem**: storage (bucket, util)
**Severity**: Informational
**Impact**: per-entry CPU overhead during bucket file serialization
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When writing a `BucketEntry` to the output file, XDR serialization should
traverse the entry structure only once. The serialized bytes and their length
should be produced in a single pass.

## Mechanism

`XDROutputFileStream::writeOne` (XDRStream.h:483-515) first calls
`xdr::xdr_size(t)` to compute the serialized size (a full recursive template
walk over the `BucketEntry` struct), then calls `xdr_argpack_archive(p, t)` to
actually serialize (a second full recursive walk). Both walks visit the same
~10-30 struct/union nodes per entry. The size is needed upfront to write the
4-byte record header before the data.

An alternative single-pass approach would serialize into a pre-allocated
buffer (large enough for the biggest expected entry), then extract the size
from the write position. However, `xdr_put` requires pre-computed bounds
(`xdr_put(begin, end)`) making this approach incompatible with the current
xdrpp library design without significant changes.

For ~10,000-16,000 entries per level-0 merge:
- `xdr_size` is pure integer arithmetic (~10-30ns per entry for BucketEntry)
- Total: 10,000 × 20ns = ~200μs per ledger

## Trigger

Every level-0 bucket merge calls `BucketOutputIterator::put()` →
`writeOne()` for each merged entry.

## Target Code

- `src/util/XDRStream.h:writeOne:483-515` — `xdr_size(t)` at line 486, `xdr_argpack_archive(p, t)` at line 501
- `lib/xdrpp/xdrpp/types.h:224-227` — `xdr_size` implementation (pure arithmetic)

## Evidence

1. Two separate traversals of the same XDR structure are visible in `writeOne`: `xdr_size(t)` and `xdr_argpack_archive(p, t)`.
2. For complex `BucketEntry` objects with nested CONTRACT_DATA SCVals, each traversal visits ~20-30 template-dispatched nodes.

## Anti-Evidence

1. `xdr_size` is pure integer arithmetic inlined by the compiler — ~10-30ns per BucketEntry (per the finding in transaction-ledger/fail/005).
2. ~200μs per ledger against 100-500ms close time is ~0.04% — far below any severity threshold.
3. Changing the approach requires modifying the xdrpp library's `xdr_put` API to support growable buffers, which is a cross-cutting change with high risk.
4. The `mBuf` vector in `writeOne` already grows-only (never shrinks), so `xdr_size` only causes a `resize` on the first few entries. The steady-state cost is just the size computation itself.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

`xdr_size` is pure integer arithmetic with no allocations, completing in
~10-30ns per BucketEntry. At ~10,000 entries per level-0 merge, the total
redundant traversal cost is ~200μs — 0.04% of ledger close time. This is
far below the Informational threshold of 1%. Additionally, eliminating the
double traversal would require changes to the xdrpp library's serialization
API, which is shared infrastructure with much broader impact than the bucket
output path.

### Lesson Learned

XDR size computation (`xdr_size`) is essentially free for individual
objects — it's a compiler-inlined recursive integer addition with no memory
access beyond the struct fields themselves. Only when called millions of
times per ledger (not thousands) would this warrant optimization. The
lesson from transaction-ledger/fail/005 (xdr_size on LedgerKey is negligible)
generalizes to BucketEntry objects as well.
