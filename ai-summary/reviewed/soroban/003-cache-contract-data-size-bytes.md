# H003: Store CONTRACT_DATA sizeBytes in InMemorySorobanState Like CONTRACT_CODE

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / repeated XDR size walks in post-close Soroban state maintenance
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The post-close in-memory Soroban state update should not need to recompute the
old XDR size of every modified `CONTRACT_DATA` entry on every ledger. Once a
contract-data entry is in `InMemorySorobanState`, its current accounted size
should be stored alongside the entry, just as contract-code entries already
store `sizeBytes`.

## Mechanism

`ContractCodeMapEntryT` caches `sizeBytes` specifically "to make the contract
code updates faster", but `ContractDataMapEntryT` stores only the entry and TTL.
As a result, `updateContractData`, `createContractDataEntry`, and
`deleteContractData` repeatedly call `xdr::xdr_size` over old/new
contract-data entries while `updateState` walks every modified Soroban entry
after each ledger close. In write-heavy apply-load runs this creates a pure
post-host tax that scales with the number of contract-data balance / pool-state
updates rather than with useful execution.

## Trigger

Run batched SAC, `custom_token`, or `soroswap` apply-load and sample
`InMemorySorobanState::updateState`, especially `updateContractData`,
`createContractDataEntry`, and `deleteContractData`. If the hypothesis is
correct, profiles will show repeated `xdr::xdr_size` work on contract-data
entries after transaction execution has already finished.

## Target Code

- `src/ledger/InMemorySorobanState.h:46-79` — contract-data entries lack a cached `sizeBytes`, while contract-code entries already keep one
- `src/ledger/InMemorySorobanState.cpp:updateContractData:92-110` — recomputes both old and new XDR sizes on every update
- `src/ledger/InMemorySorobanState.cpp:createContractDataEntry:114-141` — recomputes XDR size on every create
- `src/ledger/InMemorySorobanState.cpp:deleteContractData:193-201` — recomputes old XDR size on every delete
- `src/ledger/InMemorySorobanState.cpp:updateState:553-597` — runs this maintenance pass on every closed ledger

## Evidence

The code already contains the precedent: contract-code state caches
`sizeBytes` because size recomputation is expensive enough to warrant stored
metadata. Contract-data updates currently lack the same optimization even
though apply-load workloads are dominated by contract-data writes, especially
batched SAC balance updates and soroswap/custom-token state mutations.

## Anti-Evidence

This does not eliminate all size work: newly written contract-data entries still
need one fresh size computation unless earlier phases thread it through.
Compared to the contract-data hash issue, this path is confined to the serial
post-close state-maintenance phase, so its ceiling is lower if host execution or
bucket writes dominate a scenario.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the `updateState` → `updateContractData` → `xdr_size` path and confirmed the redundant recomputation exists. However, the actual cost is dominated by other operations in the same function — particularly the SHA256 hash computation in `InternalContractDataMapEntry::hash()/copyKey()` (which calls `getTTLKey` → SHA256) and the `unique_ptr` allocation/deallocation during erase+re-insert. The `xdr_size` calls represent a small fraction of the total `updateContractData` cost, and `updateContractData` itself is a small fraction of total ledger close time.

### Code Paths Examined

- `src/ledger/InMemorySorobanState.cpp:92-111` — `updateContractData`: calls `xdr_size` twice (old entry line 102, new entry line 103), but also erases and re-inserts into unordered_set (lines 108-110), which triggers SHA256 hash recomputation via `ValueEntry::copyKey()` → `getTTLKey()`
- `src/ledger/InMemorySorobanState.cpp:114-142` — `createContractDataEntry`: one `xdr_size` call (line 138) for the new entry — caching would not help here
- `src/ledger/InMemorySorobanState.cpp:192-201` — `deleteContractData`: one `xdr_size` call (line 199) for old entry — caching saves this
- `src/ledger/InMemorySorobanState.h:136-174` — `ValueEntry::copyKey()` and `hash()` call `getTTLKey(LedgerEntryKey(*entry.ledgerEntry))` which computes SHA256 — this dwarfs `xdr_size` cost
- `src/ledger/LedgerTypeUtils.cpp:51-74` — `ledgerEntrySizeForRent`: for CONTRACT_DATA (non-code entries), simply returns `entryXdrSize` with no FFI call, unlike CONTRACT_CODE which requires `rust_bridge::contract_code_memory_size_for_rent`
- `lib/xdrpp/xdrpp/types.h:223-227` — `xdr_size` is a recursive `serial_size` walk with no allocation

### Findings

**The inefficiency exists** — `updateContractData` calls `xdr_size` on both old and new entries when it only needs the delta for `updateStateSizeOnEntryUpdate`. Caching the old size in `ContractDataMapEntryT` would eliminate one `xdr_size` call per update and one per delete.

**Critical context the hypothesis misses:** The reason `ContractCodeMapEntryT` caches `sizeBytes` is NOT primarily to avoid `xdr_size`. The comment at line 65-70 of the header says it's to "make the contract code updates faster" AND "make them more resilient to protocol and config upgrades." For CONTRACT_CODE, `contractCodeSizeForRent` calls `rust_bridge::contract_code_memory_size_for_rent` — an FFI call crossing into Rust that computes the in-memory compiled module size. That FFI call is orders of magnitude more expensive than `xdr_size`. For CONTRACT_DATA, `ledgerEntrySizeForRent` just returns the raw XDR size with no FFI call.

**Impact estimate:** For SAC at 3200 txs/ledger, ~3200 CONTRACT_DATA updates. Each `xdr_size` on a SAC balance entry (~200-300 bytes XDR) takes ~50-100ns. Savings: ~3200 calls × ~75ns ≈ 240μs per ledger. The `updateContractData` function's total cost per call is ~800-1000ns (dominated by SHA256 hash in erase+re-insert), so `xdr_size` accounts for ~15-20% of that function's time. But `updateContractData` total time is ~3.2ms per ledger against ~100ms+ total close time — so the optimization saves ~0.2-0.3% of total close time.

**The fix is correct** — adding a `uint32_t sizeBytes` field to `ContractDataMapEntryT` (and its `ValueEntry` wrapper) is safe. No callers depend on the absence of this field. No thread-safety concerns since this is only mutated in the sequential commit phase. Memory cost is negligible (4 bytes per entry).

### PoC Guidance

- **Target code**: `src/ledger/InMemorySorobanState.h` (add `sizeBytes` to `ContractDataMapEntryT` and `ValueEntry`), `src/ledger/InMemorySorobanState.cpp` (update `updateContractData`, `createContractDataEntry`, `deleteContractData` to use cached size)
- **Change description**: Add `uint32_t sizeBytes` to `ContractDataMapEntryT`. In `createContractDataEntry`, compute `xdr_size` once and store it. In `updateContractData`, read old size from cache, compute new size, store new size. In `deleteContractData`, read old size from cache instead of recomputing.
- **Correctness check**: Existing tests for `InMemorySorobanState` (search for `InMemorySorobanState` in test files). The `checkUpdateInvariants()` call at end of `updateState` provides a basic sanity check. Run `[soroban]` tagged tests.
- **Benchmark focus**: The improvement is expected to be sub-1% of total ledger close time. Profile `InMemorySorobanState::updateContractData` specifically to confirm the `xdr_size` cost reduction. Unlikely to show up in coarse-grained benchmark metrics.
