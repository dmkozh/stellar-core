# H001: Cache TTL Key Hash in InternalContractDataMapEntry::ValueEntry

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Low
**Impact**: CPU reduction in Soroban entry lookups during parallel apply
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When looking up a ContractData entry in `InMemorySorobanState` via `mContractDataEntries.find()`, the `unordered_set` should compute the hash of the stored entry in O(1) using a cached value, and equality comparison should use the cached hash directly. The total cost of a lookup should be dominated by the hash table probe, not by cryptographic hashing.

## Mechanism

`InternalContractDataMapEntry::ValueEntry::hash()` calls `copyKey()` which calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))`. The `getTTLKey` function (LedgerTypeUtils.cpp:36) computes `sha256(xdr::xdr_to_opaque(e))` — a full SHA-256 hash plus XDR serialization — on **every** call. This means every `unordered_set` operation (find, insert, erase, rehash) on a `ValueEntry` triggers a fresh SHA-256 computation for the stored entry. Furthermore, `operator==` in `AbstractEntry` calls `copyKey()` on **both** operands, so collision resolution during lookups triggers two additional SHA-256 computations per bucket probe.

Note: This hypothesis is complementary to `transaction-ledger/002-cache-getttlkey-sha256` which addresses call-site caching in `ParallelApplyUtils`. This hypothesis targets the **internal data structure** overhead — the `unordered_set` that stores the entire in-memory Soroban state pays SHA-256 costs on every structural operation (insert, erase, rehash during `updateState`), and on equality checks during find. The fix is to cache the `uint256` TTL key hash inside `ValueEntry` at construction time.

## Trigger

Run the apply-load benchmark with any Soroban scenario. Operations on `mContractDataEntries` occur during:
- `InMemorySorobanState::updateState()` — called once per ledger to merge init/live/dead entries
- `InMemorySorobanState::get()` — called per-entry during parallel Soroban apply for entries not in thread/global maps
- `initializeStateFromSnapshot()` — called at startup

## Target Code

- `src/ledger/InMemorySorobanState.h:ValueEntry::copyKey():148-153` — recomputes SHA-256 on every call
- `src/ledger/InMemorySorobanState.h:ValueEntry::hash():155-158` — delegates to copyKey()
- `src/ledger/InMemorySorobanState.h:AbstractEntry::operator==():127-131` — calls copyKey() on both sides
- `src/ledger/LedgerTypeUtils.cpp:31-38` — getTTLKey computes sha256(xdr_to_opaque(e))
- `src/ledger/InMemorySorobanState.cpp:53-63` — updateContractDataTTL erases+reinserts (2 hash computations)
- `src/ledger/InMemorySorobanState.cpp:92-130` — updateContractData calls find (hash computation)

## Evidence

1. `ValueEntry::copyKey()` explicitly calls `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` which computes `sha256(xdr::xdr_to_opaque(e))`.
2. The `hash()` method delegates to `copyKey()`, confirming no caching exists.
3. `operator==` calls `copyKey()` on both operands — two SHA-256 computations per equality check.
4. `updateContractDataTTL` (line 53-63) does erase+reinsert, paying hash cost twice.
5. The `QueryKey` class already demonstrates the pattern of storing the hash directly — `ValueEntry` should do the same.

## Anti-Evidence

1. `QueryKey` lookups (from `InMemorySorobanState::get`) only trigger `hash()` on the QueryKey side (which has the cached hash) — the `ValueEntry::hash()` is not called during find. Only `operator==` is called on bucket collision, which calls `copyKey()` on the ValueEntry.
2. With good hash distribution, collisions should be rare, so the `operator==` path is rarely hit.
3. The `updateState` path runs once per ledger and is sequential — it's not in the per-tx hot path.
4. If the `InMemorySorobanState::get()` call path is rarely reached (because entries are pre-loaded into thread maps), the impact may be smaller than expected.
