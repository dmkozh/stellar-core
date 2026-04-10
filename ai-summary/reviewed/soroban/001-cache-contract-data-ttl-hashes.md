# H001: Cache CONTRACT_DATA TTL Hashes Instead of Re-SHA256ing Keys on Every Lookup

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / repeated XDR+SHA256 work in in-memory Soroban lookups and updates
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a `CONTRACT_DATA` entry is resident in `InMemorySorobanState`, subsequent
lookup, update, TTL lookup, and erase operations should use a stored lookup key
for that entry rather than re-encoding the full `LedgerKey` and hashing it
again. Hot-path reads during parallel apply should be dominated by actual state
access, not by recomputing the same TTL-key hash from the same contract-data
key shape on every access.

## Mechanism

`InternalContractDataMapEntry` indexes contract-data entries by the TTL-key
hash, but it does not store that hash. Both `ValueEntry::copyKey()` and the
`QueryKey(LedgerKey)` constructor derive it on demand through
`getTTLKey(...)`, which calls `sha256(xdr::xdr_to_opaque(e))` on the full
contract-data key. That makes every `InMemorySorobanState::get(CONTRACT_DATA)`
lookup and every contract-data update/delete path pay extra XDR+SHA256 work
before the unordered-set lookup, even though the hash is stable for the life of
the entry.

## Trigger

Run apply-load `custom_token` or `soroswap`, especially `T=8`, and sample CPU
in `InMemorySorobanState::get`, `InternalContractDataMapEntry::hash`, and
`getTTLKey`. If the hypothesis is correct, profiles will show repeated
`sha256(xdr_to_opaque(contractDataKey))` work for hot balance / allowance /
pool-state keys that are looked up across many transactions in a ledger.

## Target Code

- `src/ledger/InMemorySorobanState.h:101-173` — `ValueEntry::copyKey()` recomputes the TTL hash from the stored `LedgerEntry`
- `src/ledger/InMemorySorobanState.h:242-248` — `QueryKey(LedgerKey)` recomputes the TTL hash for every `CONTRACT_DATA` lookup
- `src/ledger/LedgerTypeUtils.cpp:getTTLKey:31-37` — derives the TTL key by `sha256(xdr::xdr_to_opaque(e))`
- `src/ledger/InMemorySorobanState.cpp:205-217` — `get(CONTRACT_DATA)` hits the polymorphic set on the parallel-apply read path
- `src/transactions/ParallelApplyUtils.cpp:723-734` — thread-state misses fall through to `InMemorySorobanState::get`

## Evidence

The code already stores extra cached metadata for contract code (`sizeBytes`) to
avoid recomputation on hot updates, but contract data keeps only
`shared_ptr<LedgerEntry const>` and `TTLData`. The contract-data set is then
forced to reconstruct its hash key from the full XDR key shape on every
query-side lookup and on every stored-entry hash calculation, even though the
map is logically keyed by a stable 32-byte hash.

## Anti-Evidence

The first insertion still has to compute the TTL hash once, and caching it
adds per-entry memory overhead. If cluster-local caches already eliminate most
`InMemorySorobanState::get(CONTRACT_DATA)` calls for a benchmark scenario, the
remaining win would come mostly from the seal-time update path rather than the
parallel-read path.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full lifecycle of `InternalContractDataMapEntry` through `InMemorySorobanState` and the parallel apply hot paths. Confirmed that `ValueEntry::copyKey()` recomputes `sha256(xdr::xdr_to_opaque(LedgerEntryKey))` on every call — this is invoked during `unordered_set::find()` equality checks (once per successful lookup) and during `hash()` for rehashing. The `QueryKey(CONTRACT_DATA)` constructor also pays one SHA256 per lookup. During parallel apply, `ThreadParallelApplyLedgerState::getLiveEntryOpt` falls through to `InMemorySorobanState::get()` for entries not in the thread-local map, and the first tx in each cluster misses for all footprint entries. Additionally, `setEffectsDeltaFromSuccessfulTx`, `setLedgerChangesFromSuccessfulOp`, and `commitChangesFromSuccessfulTx` each call `getLiveEntryOpt` independently, multiplying the SHA256 cost. The `updateState` path at ledger end also triggers the same pattern for every modified/created/deleted CONTRACT_DATA entry.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.h:148-158` — `ValueEntry::copyKey()` and `hash()` both call `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` which does `sha256(xdr::xdr_to_opaque(key))` every invocation
- `src/ledger/InMemorySorobanState.h:242-248` — `InternalContractDataMapEntry(LedgerKey)` constructor calls `getTTLKey(ledgerKey)` for CONTRACT_DATA keys, 1 SHA256 per query construction
- `src/ledger/LedgerTypeUtils.cpp:30-37` — `getTTLKey(LedgerKey)` serializes key via `xdr::xdr_to_opaque(e)` then SHA256s the result
- `src/ledger/InMemorySorobanState.cpp:204-217` — `get(CONTRACT_DATA)` constructs query + calls `find()`: 2 SHA256 per successful lookup (1 for query, 1 for equality check on matched ValueEntry)
- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData`: ~3 SHA256 (find + erase + emplace)
- `src/ledger/InMemorySorobanState.cpp:114-141` — `createContractDataEntry`: ~4 SHA256 (find + getTTLKey + emplace)
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `ThreadParallelApplyLedgerState::getLiveEntryOpt`: misses `mThreadEntryMap` for soroban entries not modified by prior txs in cluster, falls through to `mInMemorySorobanState.get(key)`
- `src/transactions/ParallelApplyUtils.cpp:790-829` — `setEffectsDeltaFromSuccessfulTx` calls `getLiveEntryOpt` for every modified key
- `src/transactions/ParallelApplyUtils.cpp:761-787` — `commitChangeFromSuccessfulTx` calls `getLiveEntryOpt` again for every modified key
- `src/transactions/ParallelApplyUtils.cpp:562-607` — `collectClusterFootprintEntriesFromGlobal` calls `getTTLKey(key)` for every soroban footprint entry (1 SHA256 each), but these only hit the global map (not InMemorySorobanState)

### Findings

**The inefficiency is real and measurable.** Key findings:

1. **Per-lookup cost**: Each `InMemorySorobanState::get(CONTRACT_DATA)` incurs 2 SHA256 calls — one in the QueryKey constructor and one in the `unordered_set::find()` equality check (which calls `ValueEntry::copyKey()`). Each SHA256 also involves `xdr::xdr_to_opaque` serialization + allocation.

2. **Multiplied by post-tx bookkeeping**: For the first tx in each cluster, `setEffectsDeltaFromSuccessfulTx` and `commitChangeFromSuccessfulTx` each call `getLiveEntryOpt` independently on every modified key, both falling through to `InMemorySorobanState::get()`. This doubles or triples the SHA256 cost per entry.

3. **Estimated volume**: For a 3200-tx SAC benchmark, estimated ~30,000-50,000 SHA256 calls across parallel apply + `updateState`, at ~500ns-1µs each (including xdr_to_opaque), totaling ~15-50ms per ledger close. As a fraction of total close time, this is ~3-10%.

4. **ContractCode not affected**: `mContractCodeEntries` already uses `uint256` hash as the map key, so it doesn't suffer from this problem. Only `mContractDataEntries` (using the polymorphic `unordered_set`) is affected.

5. **Thread-local map mitigates but doesn't eliminate**: After the first tx's modified entries are committed to `mThreadEntryMap`, subsequent txs in the same cluster hit the local map. But read-only footprint entries that are never modified continue to fall through on every access.

### PoC Guidance

- **Target code**: `src/ledger/InMemorySorobanState.h` — `ValueEntry` struct within `InternalContractDataMapEntry`
- **Change description**: Add a `uint256 mCachedKeyHash` member to `ValueEntry`, computed once in the constructor from `getTTLKey(LedgerEntryKey(*entry.ledgerEntry)).ttl().keyHash`. Change `copyKey()` to return `mCachedKeyHash` and `hash()` to return `std::hash<uint256>{}(mCachedKeyHash)`. Update `clone()` to copy the cached hash. Also update `updateContractDataTTL` (InMemorySorobanState.cpp:53-63) to pass the pre-computed hash when creating the replacement entry (avoid recomputing after erase+re-insert).
- **Correctness check**: Existing tests covering `InMemorySorobanState` (search for `InMemorySorobanState` in test files), plus `[soroban]` and `[tx]` test tags. The change preserves immutability semantics — the hash is stable for the entry's lifetime.
- **Benchmark focus**: Run `apply-load` with `custom_token` or `soroswap` at T=8. Measure total ledger close time. Expected improvement: 15-50ms per ledger (3-10% depending on scenario). Profile `getTTLKey` and `sha256` call counts to confirm reduction.
