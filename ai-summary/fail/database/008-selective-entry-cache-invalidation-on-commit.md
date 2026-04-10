# H008: Selective Entry Cache Invalidation Instead of Full Clear on commitChild

**Date**: 2026-04-10
**Subsystem**: database
**Severity**: Low
**Impact**: reduced BucketListDB reads via cache reuse
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

After `LedgerTxnRoot::Impl::commitChild` commits a ledger delta, only
the entries that were actually modified should be evicted from `mEntryCache`.
Unchanged entries (source accounts reused across ledgers, config entries)
should remain cached so the next ledger's `prefetchTxSourceIds` gets cache
hits instead of re-reading from BucketListDB.

## Mechanism

`commitChild` unconditionally calls `mEntryCache.clear()` (line 2974),
discarding all cached entries. On the next ledger, `prefetchTxSourceIds`
must re-read every source account from BucketListDB. If we instead
selectively evicted only the keys present in the committed delta, unchanged
entries would remain in the `RandomEvictionCache` for the next ledger's
prefetch, saving per-entry BucketListDB lookups.

## Trigger

Run any apply-load benchmark scenario. Each ledger close clears the full
entry cache, then the next ledger re-prefetches the same source accounts.

## Target Code

- `src/ledger/LedgerTxn.cpp:2974` — `mEntryCache.clear()` in commitChild
- `src/ledger/LedgerTxn.cpp:3044-3100` — `prefetch()` fills the cache from BucketListDB
- `src/ledger/LedgerManagerImpl.cpp:2340-2356` — `prefetchTxSourceIds` triggers prefetch

## Evidence

The entry cache has capacity for 100,000 entries (`ENTRY_CACHE_SIZE`),
far exceeding the ~3000 source accounts in a typical Soroban benchmark
ledger. Preserving the cache across ledger closes would provide immediate
cache hits for unchanged entries.

## Anti-Evidence

In the Soroban apply-load benchmark, every source account is modified every
ledger (sequence number bump + fee deduction during `processFeesSeqNums`).
This means ~3000 out of ~3000 cached entries are modified, leaving
essentially zero unchanged entries to preserve. Selective invalidation would
produce the same result as full clear.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (relates to fail/006's cache iteration but this is about cache REUSE, not iteration cost)

### Why It Failed

The optimization's premise — that many cached entries remain unchanged
across ledger closes — is false for the target benchmark. In Soroban
apply-load, fee processing modifies every source account every ledger
(sequence number + balance change via `processFeeSeqNum`). With ~3000
Soroban transactions using ~3000 unique source accounts, effectively
100% of cached entries become stale after each ledger. Selective
invalidation would evict all ~3000 entries and preserve zero entries,
providing no benefit over the current full clear.

### Lesson Learned

Cache reuse optimizations must account for the write pattern of the
target workload. In the Soroban apply-load benchmark, source accounts
are single-use per ledger (each account submits one transaction, which
bumps its sequence number). Future agents should check if the same
entries are actually reused unchanged across ledger closes before
proposing cache preservation strategies.
