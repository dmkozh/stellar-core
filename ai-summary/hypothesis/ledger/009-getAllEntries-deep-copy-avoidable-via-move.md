# H009: getAllEntries Deep-Copies LedgerEntries When Move Semantics Are Safe Post-Seal

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: Reduced allocation and copy overhead in the ledger close critical path for all Soroban scenarios
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After a `LedgerTxn` is sealed (via `getAllEntries` or `getChanges`), the internal `mEntry` map is never read again — the LedgerTxn is consumed and will be destroyed. Therefore, `getAllEntries` should move `LedgerEntry` objects out of the `mEntry` map's `LedgerEntryPtr` shared_ptrs into the output vectors, avoiding deep copies of XDR union objects. The output vectors should contain entries that were transferred at near-zero cost (pointer/integer swaps) rather than copied (full XDR tree duplication).

## Mechanism

`LedgerTxn::Impl::getAllEntries` (LedgerTxn.cpp:1627-1668) iterates all entries in `mEntry` after sealing and copies them into three output vectors:

```cpp
resInit.emplace_back(entry->ledgerEntry());   // deep copy
resLive.emplace_back(entry->ledgerEntry());   // deep copy
resDead.emplace_back(key.ledgerKey());        // deep copy (smaller)
```

`entry->ledgerEntry()` returns `LedgerEntry const&`, forcing a copy constructor call. Each `LedgerEntry` is an XDR union containing a `LedgerEntryData` (which itself can be one of `AccountEntry`, `TrustLineEntry`, `OfferEntry`, `DataEntry`, `ClaimableBalanceEntry`, `LiquidityPoolEntry`, `ContractDataEntry`, `ContractCodeEntry`, or `TTLEntry`). For Soroban entries (CONTRACT_DATA, CONTRACT_CODE, TTL), the copy cost includes:
- CONTRACT_DATA: copying the `SCVal` key and value (which may contain nested vectors/maps)
- CONTRACT_CODE: copying the Wasm bytecode blob (can be large, but rare on non-upload ledgers)
- TTL: small fixed-size struct (cheap copy)

The lambda passed to `maybeUpdateLastModifiedThenInvokeThenSeal` receives `EntryMap const&`, preventing move operations. However, since:
1. After sealing, `mEntry` is never read again
2. The `LedgerTxn` will be destroyed shortly after
3. The `LedgerEntryPtr` holds a `shared_ptr<InternalLedgerEntry>`, so if the shared_ptr is the only owner, we could move the underlying `LedgerEntry` out

The fix would be to add a `getAllEntriesMoving` overload (or modify the existing one) that takes `EntryMap&` (non-const) and uses `std::move(entry->ledgerEntry())` or equivalent. Alternatively, the method could be changed to return `vector<shared_ptr<InternalLedgerEntry>>` with entry state metadata, avoiding copies entirely by sharing ownership.

For the SAC benchmark with ~10,000-20,000 modified Soroban entries per ledger, eliminating deep copies could save 2-10ms per ledger close (estimated ~100-500ns per entry copy × 10,000-20,000 entries).

## Trigger

Run the SAC apply-load benchmark at T=1 or T=8. Profile the `getAllEntries` call within the `finalizeLedgerTxnChanges` Tracy zone. Measure:
1. Time spent in `getAllEntries` specifically
2. Number of entries extracted (should match modified entry count)
3. Compare with a move-based version of the same function

## Target Code

- `src/ledger/LedgerTxn.cpp:1627-1668` — `getAllEntries` implementation with deep-copy loop
- `src/ledger/LedgerTxn.cpp:1637` — Lambda takes `EntryMap const&`, preventing moves
- `src/ledger/LedgerTxn.cpp:2333-2351` — `maybeUpdateLastModifiedThenInvokeThenSeal` signature forces const reference
- `src/ledger/InternalLedgerEntry.h:158-159` — `ledgerEntry()` only has `const&` and `&` overloads, no `&&` overload
- `src/ledger/LedgerManagerImpl.cpp:3039-3046` — Consumers of `getAllEntries` output vectors

## Evidence

1. After `maybeUpdateLastModifiedThenInvokeThenSeal` sets `mIsSealed = true` (LedgerTxn.cpp:2345), the `mEntry` map is never read again — no method uses it after sealing except the destructor (which destroys it).
2. The `LedgerEntryPtr` holds a `shared_ptr<InternalLedgerEntry>`, and in the root-level LedgerTxn used for `finalizeLedgerTxnChanges`, these shared_ptrs have refcount=1 (no child LedgerTxn shares them at this point because all children have already committed).
3. `InternalLedgerEntry` already has a move constructor (`assign(InternalLedgerEntry&&)` — InternalLedgerEntry.h:50), so moving is well-supported by the type.
4. The `getDelta()` method (LedgerTxn.cpp:1415-1434) already avoids deep copies explicitly: "Deep copy is not required here because getDelta causes LedgerTxn to enter the sealed state, meaning subsequent modifications are impossible." The same reasoning applies to `getAllEntries` but the optimization was not applied there.
5. The three output vectors are consumed by `addLiveBatch`, `updateInMemorySorobanState`, and `addAnyContractsToModuleCache` — all of which only need `const&` access, so move semantics in extraction doesn't affect consumers.

## Anti-Evidence

1. **Shared_ptr refcount**: If any other code holds a reference to the `shared_ptr<InternalLedgerEntry>` inside `LedgerEntryPtr`, moving the underlying `LedgerEntry` would corrupt those references. Need to verify refcount=1 at the point of `getAllEntries` call.
2. **API change complexity**: Changing `maybeUpdateLastModifiedThenInvokeThenSeal` to accept non-const lambda requires updating multiple callers (commit, getChanges, getDelta, getAllEntries, getAllKeysWithoutSealing).
3. **Entry size varies**: For the SAC benchmark, most modified entries are CONTRACT_DATA (balance values) which are relatively small. The copy cost per entry may be only ~100-300ns, making total savings 2-6ms — potentially below the Medium threshold.
4. **Reserve over-allocation**: The three `reserve(mEntry.size())` calls (lines 1634-1636) waste memory but this is a separate concern from the copy cost.
