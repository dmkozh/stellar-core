# H007: Add `sizeBytes` to ContractDataMapEntryT and stop recomputing old XDR sizes

**Date**: 2026-04-10
**Subsystem**: storage (ledger)
**Severity**: Low
**Impact**: per-ledger CPU reduction in contract-data state-size accounting
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When a cached `CONTRACT_DATA` entry is updated or deleted, the old size used for
in-memory state accounting should come from cached metadata already stored next
to the entry, just as contract-code entries already do. The code should not
re-walk the old XDR object merely to recover its previous byte size.

## Mechanism

`ContractDataMapEntryT` stores only `ledgerEntry` and `ttlData`, while
`ContractCodeMapEntryT` explicitly stores `sizeBytes` to make updates faster.
As a result, `updateContractData` and `deleteContractData` recompute the old
size with `xdr::xdr_size(*dataIt->get().ledgerEntry)` before adjusting
`mContractDataStateSize`. On Soroban-heavy workloads this means every contract
data update/delete performs an extra traversal of the old `SCVal` payload even
though the old size is stable once the entry is inserted into the cache.

## Trigger

Run SAC, custom-token, or soroswap apply-load scenarios with many balance,
allowance, reserve, or pool-state updates. Each ledger close that updates or
deletes many `CONTRACT_DATA` entries will re-run `xdr_size` over the old value
for bookkeeping.

## Target Code

- `src/ledger/InMemorySorobanState.h:46-80` — `ContractDataMapEntryT` lacks `sizeBytes`, while `ContractCodeMapEntryT` already caches it for speed
- `src/ledger/InMemorySorobanState.cpp:92-110` — `updateContractData` recomputes both old and new sizes
- `src/ledger/InMemorySorobanState.cpp:192-201` — `deleteContractData` recomputes old size before erase
- `src/ledger/InMemorySorobanState.cpp:138-141` — creates contract-data entries without persisting the computed size

## Evidence

The codebase already uses cached entry-size metadata for contract code and
documents the reason: faster updates and resilience across upgrades. Contract
data does not need the same protocol-dependent rent-size adjustment as contract
code, so caching its XDR byte size is simpler: compute once on create/update,
reuse on later update/delete, and only recompute the new size for the incoming
value.

## Anti-Evidence

This only removes the old-size traversal; the new value still needs one
`xdr::xdr_size(...)` call on every create/update, so it is not a full
elimination. If apply-load contract-data entries are tiny in practice, the saved
CPU may be modest. The change also adds a 4-byte field per cached contract-data
entry, so there is a small memory tradeoff.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not a duplicate of prior investigations (fail/003 covers `xdr_size` in BucketOutputIterator, fail/004 covers erase+emplace rehash, fail/006 covers TTL key hash caching; none target this specific `sizeBytes` caching proposal)
**Failed At**: reviewer

### Trace Summary

Traced the `updateContractData` (lines 92–111) and `deleteContractData` (lines 192–201) code paths in `InMemorySorobanState.cpp`. Both call `xdr::xdr_size()` on the old `LedgerEntry` to compute the previous size for `mContractDataStateSize` accounting. The hypothesis proposes caching this value like `ContractCodeMapEntryT::sizeBytes` does. However, `xdr_size()` is pure integer arithmetic (template-inlined recursive field-size summation with no allocations or memory access beyond the struct itself), and the serial `updateState()` path processes far fewer entries than the total transaction count.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData()`: calls `xdr::xdr_size(*dataIt->get().ledgerEntry)` at line 102 (old size) and `xdr::xdr_size(ledgerEntry)` at line 103 (new size). Only the old-size call would be eliminated by caching.
- `src/ledger/InMemorySorobanState.cpp:192-201` — `deleteContractData()`: calls `xdr::xdr_size(*it->get().ledgerEntry)` at line 199. Would be eliminated by caching.
- `src/ledger/InMemorySorobanState.cpp:138-141` — `createContractDataEntry()`: calls `xdr::xdr_size(ledgerEntry)` at line 138. This is a new entry, so the size must always be computed — no savings here.
- `src/ledger/InMemorySorobanState.cpp:536-598` — `updateState()`: iterates `initEntries`, `liveEntries`, and `deadEntries` from the ledger close, dispatching to create/update/delete for CONTRACT_DATA entries.
- `lib/xdrpp/xdrpp/types.h:224-227` — `xdr_size()` resolves to `xdr_traits<T>::serial_size(t)`, which is pure compile-time-dispatched integer arithmetic.
- `src/ledger/InMemorySorobanState.cpp:19-34` — `contractCodeSizeForRent()`: contract code uses cached `sizeBytes` because its size computation involves `ledgerEntrySizeForRent()` which calls into the Soroban host for protocol-dependent module size — a much more expensive operation than plain `xdr_size()`.

### Why It Failed

**The inefficiency exists but is not in a hot path — it executes too infrequently and too cheaply to produce a measurable improvement.**

1. **`xdr_size()` is essentially free.** It is pure integer arithmetic with no allocations, no I/O, and no cryptographic computation. Prior investigation (fail/003) established `xdr_size` completes in ~10–30ns per entry for complex XDR objects like `BucketEntry`. `LedgerEntry` is comparable. This is fundamentally different from the SHA-256 computation in `getTTLKey()` (~200–500ns) or the Soroban host call in `contractCodeSizeForRent()`.

2. **The serial `updateState()` path processes far fewer entries than expected.** Prior investigation (fail/006) established that the parallel apply path pre-loads footprint entries via `collectClusterFootprintEntriesFromGlobal()`, so the serial `updateState()` call processes only ~100–300 entries per ledger, not the ~6,400–9,600 implied by the hypothesis.

3. **Total savings: ~2–6μs per ledger.** At ~200 CONTRACT_DATA updates/deletes per ledger × ~20ns per `xdr_size` call = ~4μs. Against a 100–500ms ledger close, this is **~0.001–0.004%** — four orders of magnitude below the Informational threshold.

4. **The analogy to `ContractCodeMapEntryT::sizeBytes` is misleading.** Contract code caches its size because `contractCodeSizeForRent()` (line 20–34) calls `ledgerEntrySizeForRent()` which invokes the Soroban host for protocol-dependent in-memory module size computation — a far more expensive operation than `xdr_size()`. The caching serves a different purpose: avoiding an expensive host call and maintaining resilience across protocol/config upgrades (as noted in the comment at lines 66–70 of the header). Contract data uses raw `xdr_size()` which has no such cost.

### Lesson Learned

Not all `xdr_size()` calls are worth caching. The function is pure compile-time-dispatched integer arithmetic (~10–30ns) — fundamentally different from cryptographic hashes (~200–500ns) or host function calls. When evaluating whether to cache a computed value, compare the computation cost against the per-entry overhead of storing and maintaining the cache. At ~20ns per call and ~200 calls per ledger, the total is ~4μs — not worth the code complexity or the 4-byte-per-entry memory increase.
