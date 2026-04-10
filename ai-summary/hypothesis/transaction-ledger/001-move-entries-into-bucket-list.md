# H001: Eliminate Redundant LedgerEntry Deep Copies in finalizeLedgerTxnChanges → addLiveBatch

**Date**: 2025-07-22
**Subsystem**: transaction-ledger (LedgerManagerImpl, LiveBucket)
**Severity**: Low
**Impact**: Reduced memory allocation overhead in ledger close path
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When committing a ledger close, the entries extracted from the LedgerTxn via
`getAllEntries` should be transferred into the BucketList with minimal copying.
Ideally, each `LedgerEntry` is deep-copied exactly once (from the LedgerTxn
entry map into the output vector) and then moved through subsequent consumers.

## Mechanism

`finalizeLedgerTxnChanges` calls `getAllEntries` which deep-copies all ~16,000
modified entries (for a 3200-tx Soroban ledger) from the LedgerTxn entry map
into `initEntries`, `liveEntries`, and `deadEntries` vectors. These vectors are
then passed as `const&` to three consumers in order:

1. `addAnyContractsToModuleCache` — reads CONTRACT_CODE entries only
2. `addLiveBatch` → `convertToBucketEntry` — deep-copies every entry AGAIN
   into `BucketEntry` objects (`ce.liveEntry() = e` on lines 394, 402)
3. `updateInMemorySorobanState` — reads entries, copies Soroban entries into
   in-memory maps

The second deep copy in `convertToBucketEntry` is unnecessary. By reordering
the calls so that `addLiveBatch` is called last, the entry vectors can be
passed by rvalue reference (`std::move`), allowing `convertToBucketEntry` to
use `ce.liveEntry() = std::move(e)` instead of copying. This eliminates ~16,000
heap allocations for XDR fields (xdr::xvector in SCVal keys/values, opaque
data, etc.).

## Trigger

Any Soroban-heavy ledger close. In the apply-load benchmark:
- 3200 SAC transfer txs × ~5 modified entries each = ~16,000 entries
- Each entry contains XDR union fields with heap-allocated vectors
- `convertToBucketEntry` copies all of them, then sorts

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:3039-3046` — call ordering of addLiveBatch vs updateInMemorySorobanState
- `src/bucket/LiveBucket.cpp:convertToBucketEntry:380-420` — the copy loop (`ce.liveEntry() = e` on lines 394, 402)
- `src/bucket/LiveBucket.cpp:freshInMemoryOnly:467-498` — calls convertToBucketEntry
- `src/bucket/BucketManager.cpp:addLiveBatch:1026-1046` — entry point, takes `const&`

## Evidence

1. `convertToBucketEntry` explicitly copies every entry: `ce.liveEntry() = e` (lines 394, 402) — this is a deep copy of the LedgerEntry XDR including all nested xdr::xvector fields.
2. After `addLiveBatch` returns, the entry vectors are only used by `updateInMemorySorobanState` (line 3045). If that call is moved before `addLiveBatch`, the vectors are free to be moved.
3. `addAnyContractsToModuleCache` (lines 3041-3042) only reads CONTRACT_CODE entries and doesn't modify the vectors, so it can remain before both.
4. The three consumers (`addAnyContractsToModuleCache`, `addLiveBatch`, `updateInMemorySorobanState`) are independent — no data flows between them, no ordering constraints.

## Anti-Evidence

1. For small entries (TTL ~40 bytes, accounts ~200 bytes), the copy cost is dominated by fixed overhead (~50ns per allocation), not data size. Total savings may be ~1-3ms.
2. The sort in `convertToBucketEntry` (line 413) dominates the function time for large entry counts, and moving vs copying doesn't affect sort cost.
3. `updateInMemorySorobanState` also copies entries into its internal maps, so one copy in the chain is unavoidable regardless.
