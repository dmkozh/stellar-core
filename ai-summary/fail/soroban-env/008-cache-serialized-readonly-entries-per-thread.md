# H002: Cache Pre-Serialized ReadOnly Footprint Entries Per Thread

**Date**: 2025-07-22
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: Eliminate redundant XDR serialization of shared readOnly entries in parallel apply
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When multiple transactions in the same parallel-apply cluster share the same
readOnly footprint entries (e.g., contract code, contract instance), each
entry should be XDR-serialized to `CxxBuf` once per thread and reused across
all transactions that reference it.

Currently, each transaction independently calls `getLedgerEntryOpt(key)` →
`toCxxBuf(*entryOpt)` for every footprint entry, performing redundant XDR
serialization of the same readOnly entries that are immutable within a cluster.

## Mechanism

In `addReads()` (InvokeHostFunctionOpFrame.cpp:360-508), for each footprint
key, the code calls `getLedgerEntryOpt(lk)` which returns a fresh
`std::optional<LedgerEntry>` (a copy), then `toCxxBuf(*entryOpt)` serializes
it via `xdr_to_opaque` into a heap-allocated `vector<uint8_t>`.

Within a parallel-apply cluster, no two TXs have read-write conflicts (by
clustering invariant). ReadOnly entries can be referenced by many TXs in the
same cluster. For example, in the SAC benchmark, all ~800 TXs per thread
(6400/8) share the same contract instance entry as readOnly. In custom_token,
all ~375 TXs share the contract code WASM entry (~10-100KB).

Each redundant serialization costs ~200ns-10μs depending on entry size. For
large contract code entries (~50KB), serialization is ~5-10μs. With 375-800
TXs per thread sharing 1-3 readOnly entries, the redundant work totals:

- SAC: ~800 TXs × ~2 shared entries × ~350ns = ~560μs per thread
- custom_token (large WASM): ~375 TXs × ~1 large entry × ~7.5μs = ~2.8ms per thread
- soroswap (multiple contracts): ~200 TXs × ~3 entries × ~3μs = ~1.8ms per thread

Against T=8 baselines (~700-900ms), this is ~0.1-0.4%. Below the 5% Low
threshold, but the custom_token scenario with a very large WASM could approach
Low territory if the WASM is >100KB.

The fix would add a per-thread `UnorderedMap<LedgerKey, std::vector<uint8_t>>`
cache populated on first serialization. Subsequent calls for the same key
would construct a `CxxBuf` via `memcpy` from the cached bytes (faster than
field-by-field XDR serialization for large entries).

## Trigger

Run apply-load with custom_token or soroswap scenarios at T=8. All TXs in
a cluster share readOnly contract code/instance entries.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:449-468` — `addReads()` inner loop where `toCxxBuf(*entryOpt)` is called per TX per entry
- `src/transactions/InvokeHostFunctionOpFrame.cpp:509-524` — `addFootprint()` calls `addReads` for readOnly and readWrite separately
- `src/transactions/InvokeHostFunctionOpFrame.cpp:316-320` — helper class declaration where cache could be added

## Evidence

- `addReads` calls `toCxxBuf` per TX per entry with no caching
- Cluster invariant guarantees readOnly entries are immutable within a cluster
- `getLedgerEntryOpt` returns a fresh copy each time (it goes through `TxParallelApplyLedgerState`)
- For WASM contracts, contract code entries can be 10-100KB+, making serialization non-trivial
- The parallel-apply framework already pre-fetches entries in `collectClusterFootprintEntriesFromGlobal` — serialization could be added there

## Anti-Evidence

- For SAC (built-in contract), the shared entries are small (~500 bytes), making savings minimal
- `memcpy` of cached bytes still costs ~2.5-5μs for large entries, so savings vs. XDR serialization are ~50% (not 100%)
- ReadWrite entries are unique per TX (by clustering invariant), so only readOnly entries benefit
- The cache adds per-thread memory overhead and code complexity
- Similar "below noise floor" optimizations have been rejected in this subsystem before
- CxxBuf requires `unique_ptr<vector<uint8_t>>` ownership, so cached bytes must be copied (not shared) into each CxxBuf

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full path from `InvokeHostFunctionParallelApplyHelper` through
`addReads()` (InvokeHostFunctionOpFrame.cpp:360-503), confirming that each TX
independently calls `getLedgerEntryOpt(lk)` → `toCxxBuf(*entryOpt)` for every
footprint key. The `toCxxBuf` template (TransactionUtils.h:372-376) calls
`xdr::xdr_to_opaque(t)` which serializes into a freshly-allocated
`std::vector<uint8_t>`. For `ContractCodeEntry`, the XDR payload is dominated
by the `code` field (`xdr::opaque_vec<>`), which is serialized as a 4-byte
length prefix followed by a raw memcpy of the WASM bytes.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:449-466` — `addReads()` inner loop: calls `getLedgerEntryOpt(lk)` then `toCxxBuf(*entryOpt)` per TX per footprint key
- `src/transactions/TransactionUtils.h:372-376` — `toCxxBuf<T>()`: wraps `xdr::xdr_to_opaque(t)` in a `CxxBuf{std::make_unique<std::vector<uint8_t>>(...)}`
- `src/transactions/ParallelApplyUtils.cpp:886-903` — `TxParallelApplyLedgerState::getLiveEntryOpt()`: looks up mTxEntryMap then falls through to ThreadParallelApplyLedgerState
- `src/transactions/ParallelApplyUtils.cpp:700-735` — `ThreadParallelApplyLedgerState::getLiveEntryOpt()`: looks up mThreadEntryMap (pre-populated via `collectClusterFootprintEntriesFromGlobal`), then InMemorySorobanState or LCL snapshot
- `src/protocol-curr/xdr/Stellar-ledger-entries.h:4232-4233` — `ContractCodeEntry` structure: `Hash hash{}; xdr::opaque_vec<> code{};` — the `code` field dominates serialization cost

### Why It Failed

The aggregate savings from caching are too small to produce a measurable
benchmark improvement, for two compounding reasons:

1. **XDR serialization of large entries is already dominated by memcpy.**
   `ContractCodeEntry` XDR consists of a small ext union, a 32-byte hash, and
   the raw WASM `opaque_vec<>`. For a 50KB WASM, ~99.9% of the serialization
   cost is the memcpy of the code bytes. Even with caching, each `CxxBuf`
   requires its own `unique_ptr<vector<uint8_t>>`, so the cached bytes must
   be memcpy'd into a new vector. The savings from caching are only the XDR
   field-traversal overhead (~100-200ns) and avoidance of vector growth
   reallocations during serialization. For a 50KB entry, actual savings per
   serialization are ~1-3μs (reallocation avoidance), not the 50% claimed.

2. **Total savings are <0.3% of baseline.** Even using the hypothesis's own
   generous estimates (~2.8ms per thread for custom_token), this represents
   0.3-0.4% of the 700-900ms T=8 baseline. With the more realistic savings
   estimate (avoiding only reallocation overhead), the actual savings would be
   ~0.5-1.5ms per thread, or ~0.1-0.2% of baseline. This is well below the
   5% Low severity threshold and within benchmark measurement noise.

The code complexity of a per-thread cache (UnorderedMap, cache population
logic, memory overhead) is not justified by unmeasurable savings.

### Lesson Learned

When evaluating serialization caching hypotheses, check whether the
serialization cost is dominated by the actual data copy (memcpy) vs. the
format traversal overhead. For XDR types with large opaque byte fields,
serialization is already essentially memcpy, and caching only avoids the
small overhead of vector growth reallocation. The cache still requires a
full memcpy into the new owned buffer, eliminating most of the potential
savings.
