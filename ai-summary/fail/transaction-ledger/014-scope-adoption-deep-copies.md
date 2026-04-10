# H014: Eliminate Deep Copies in Scope Adoption During Parallel Apply State Transitions

**Date**: 2025-07-22
**Subsystem**: transaction-ledger (ParallelApplyUtils, LedgerEntryScope)
**Severity**: Low
**Impact**: Reduced memory copy overhead in parallel apply path
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When entries flow between parallel apply state tiers (Global → Thread → Tx →
Thread → Global), immutable entries should be shared by reference rather than
deep-copied at each scope boundary.

## Mechanism

`scopeAdoptEntryOptFrom` (LedgerEntryScope.cpp:444-457) copies the
`LedgerEntry` data from the source scope into a new `ScopedLedgerEntryOpt` for
the destination scope. This creates a full deep copy of every XDR field. During
parallel apply, entries traverse 4 scope boundaries:

1. Global → Thread (`collectClusterFootprintEntriesFromGlobal`, line 587)
2. Thread → Tx (`TxParallelApplyLedgerState::getLiveEntryOpt`, line 901)
3. Tx → Thread (`commitChangesFromSuccessfulTx`, line 839)
4. Thread → Global (`commitChangesFromThreads`, line 546-559)

For 3200 txs × ~5 entries each, this results in ~64,000 scope adoption copies.
Using `shared_ptr<const LedgerEntry>` for read-only entries would eliminate
copies at transitions 1-2.

## Trigger

Any Soroban parallel apply phase with multiple scope transitions.

## Target Code

- `src/ledger/LedgerEntryScope.cpp:scopeAdoptEntryOptFromImpl:444-457` — copies entry.mEntry
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:563-608` — Global→Thread
- `src/transactions/ParallelApplyUtils.cpp:getLiveEntryOpt:700-735` — Thread→Tx
- `src/transactions/ParallelApplyUtils.cpp:commitChangesFromSuccessfulTx:832-843` — Tx→Thread

## Evidence

1. Each scope adoption invokes copy constructor for `optional<LedgerEntry>` including all nested XDR fields.
2. Read-only footprint entries don't need copies — they're never modified.
3. For CONTRACT_CODE entries with Wasm (~20KB), copies are expensive.

## Anti-Evidence

1. Most entries are small (TTL ~40 bytes, accounts ~200 bytes, contract data ~100-300 bytes). Copy cost is ~50-100ns per entry.
2. Total copy cost: ~64,000 × ~100ns ≈ 6.4ms. But deduplication reduces this — `collectClusterFootprintEntriesFromGlobal` uses a map check (line 578) so shared entries across txs are copied once. Realistic count: ~500-2000 unique entries × 4 transitions × ~100ns ≈ 0.2-0.8ms.
3. The scope system was designed specifically for safety — it ensures entries can't be modified through stale references. Switching to shared_ptr undermines this safety guarantee and would require redesigning the scope checking system.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The deduplication in `collectClusterFootprintEntriesFromGlobal` (checking
`mThreadEntryMap.find(key)` before copying) means the actual number of scope
adoptions is bounded by unique footprint keys per cluster (~500-2000), not
total footprint keys across all txs (~16000). With small entry sizes (100-200
bytes average), the total copy overhead is ~0.2-0.8ms per ledger close — well
below the 3-5% threshold for Low severity. Additionally, changing the scope
system to use shared_ptr would be a fundamental redesign that contradicts the
safety-by-copying invariant the system was built around.

### Lesson Learned

The scope adoption dedup check in `collectClusterFootprintEntriesFromGlobal`
effectively limits the number of copies to unique keys per cluster, not total
keys across all txs. Always check for deduplication before estimating copy
counts in scope transition paths.
