# H013: Combine `maybeUpdateLastModified` and `getAllEntries` Into Single Pass

**Date**: 2026-04-10
**Subsystem**: ledger (LedgerTxn)
**Severity**: Low
**Impact**: Eliminate one hash-map traversal over ~30K-60K entries during ledger seal
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `getAllEntries` seals the LedgerTxn, it should iterate the `mEntry`
hash map only once — updating `lastModifiedLedgerSeq` and classifying
entries into init/live/dead vectors in a single pass.

## Mechanism

`LedgerTxn::Impl::getAllEntries` (LedgerTxn.cpp:1637) calls
`maybeUpdateLastModifiedThenInvokeThenSeal` which does TWO separate passes
over `mEntry`:

1. **Pass 1** — `maybeUpdateLastModified()` (lines 2318-2329): iterates
   ALL entries in `mEntry`, checking `!isDeleted()` and
   `mShouldUpdateLastModified`, then writes `lastModifiedLedgerSeq`.

2. **Pass 2** — The callback `f(mEntry)` (lines 1638-1663): iterates ALL
   entries again, filtering by `LEDGER_ENTRY` type and classifying into
   `resInit`, `resLive`, `resDead` vectors.

For ~30,000-60,000 entries in an `UnorderedMap` (pointer-chased linked
list buckets), two sequential traversals cause:
- Double pointer-chasing through bucket chains (~10-30ns per node × 2)
- Potential L1/L2 cache thrashing between passes (entry data ~60-120 bytes
  per node, total ~2-7MB — may exceed L2)

Combining both operations into a single pass would halve the traversal cost
and improve cache locality.

## Trigger

Profile `getAllEntries` with a Tracy zone or `perf record`. Measure the
time spent in `maybeUpdateLastModified` vs. the classification callback.
A single-pass variant should show measurable wall-time reduction.

## Target Code

- `src/ledger/LedgerTxn.cpp:maybeUpdateLastModified:2311-2330` — first pass over mEntry
- `src/ledger/LedgerTxn.cpp:maybeUpdateLastModifiedThenInvokeThenSeal:2333-2351` — orchestrates two passes
- `src/ledger/LedgerTxn.cpp:getAllEntries:1627-1668` — callback does second pass

## Evidence

- Both passes iterate the same `mEntry` map using the same iterator pattern
- The `maybeUpdateLastModifiedThenInvokeThenSeal` API forces the two-pass
  pattern by design (first updates, then invokes callback)
- The same two-pass pattern is used by `getChanges` (line 1360) and
  `getDelta` (line 1415) — multiple consumers affected
- For 60,000 entries at ~20ns average per iteration: ~1.2ms per pass × 2 =
  ~2.4ms total; saving one pass: ~1.2ms

## Anti-Evidence

- The `maybeUpdateLastModifiedThenInvokeThenSeal` API serves multiple
  callers (`getAllEntries`, `getChanges`, `getDelta`), each with different
  classification logic — combining would require duplicating the
  `lastModified` update in each caller
- The entry map iteration is ~20ns per node (mostly pointer chasing), and
  with ~30K-60K entries, one pass saves ~0.6-1.2ms — a small fraction of
  total close time
- Cache effects may be minimal: the second pass may find entry data still
  in L3 from the first pass (both happen within a few ms)
- The code separation is intentional for maintainability — `maybeUpdateLastModified`
  is a well-defined operation that shouldn't be mixed with caller-specific logic
- The `mShouldUpdateLastModified` flag means the first pass is a no-op for
  some LedgerTxn instances (avoiding even the branch overhead)

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

Estimated savings of ~0.6-1.2ms per ledger close represent <1% of total
close time (100ms-2s). The API refactoring cost is significant: either
duplicate `lastModified` logic in each of the 4 callers of
`maybeUpdateLastModifiedThenInvokeThenSeal`, or create a fused API that
couples update semantics with extraction logic. The code is well-structured
with clear separation of concerns, and the hash-map second-pass cache
effects are likely mitigated by L3 residency from the first pass.

### Lesson Learned

Hash-map traversals over ~30K-60K entries cost ~0.6-1.2ms per pass. Two
sequential passes over the same map with similar access patterns benefit
from L3 cache warming, so the second pass is not 2× the cost of one pass.
Optimizing this pattern requires restructuring well-factored APIs for
marginal (<1%) improvement — generally not worthwhile unless the map is
much larger or the per-entry work is heavier.
