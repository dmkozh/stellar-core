# H001: Reuse RO Entry CxxBufs Across TXs Within a Parallel Apply Cluster

**Date**: 2026-04-10
**Subsystem**: soroban-env (bridge layer), transactions
**Severity**: Low
**Impact**: Eliminate per-TX heap allocation + memcpy for cached read-only ledger entries
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When processing multiple transactions in the same parallel-apply cluster that
share identical read-only footprint entries (contract code, contract instance),
the serialized RO entry bytes should be prepared once and passed to every
subsequent bridge invocation without copying. Each `invoke_host_function` call
takes `ledger_entries: &Vec<CxxBuf>` **by reference** — the Rust side borrows
the data and does not consume or move the CxxBuf contents. After the call
returns, the CxxBuf objects remain valid and could be reused.

## Mechanism

Currently, `InvokeHostFunctionParallelApplyHelper::serializeLedgerEntryForBridge`
(line 1193–1210) caches the **bytes** of RO entries in the per-thread
`mRoSerializationCache`, but on every TX it constructs a **new** `CxxBuf` by
copying those cached bytes:

```cpp
return CxxBuf{std::make_unique<std::vector<uint8_t>>(it->second.first)};
```

This invokes `std::vector<uint8_t>`'s copy constructor, which heap-allocates a
new buffer and memcpy's the cached bytes. For large contract code entries, this
is significant:

- `token.wasm`: 46,535 bytes → ~3–5 μs per copy (alloc + memcpy + dealloc)
- Soroswap total (router+pool+factory+token): ~118 KB → ~8–14 μs per TX

The same pattern applies to `serializeTtlEntryForBridge` (line 1212–1231).

Since `invoke_host_function` takes entries by `&Vec<CxxBuf>` (Rust reference),
the CxxBufs survive the call. If RO entries were stored in a separate
`rust::Vec<CxxBuf>` at the thread level (built once per cluster), and the bridge
accepted separate RO/RW entry vectors, these copies would be eliminated for all
but the first TX in each cluster.

## Trigger

Run the apply-load benchmark with `custom_token` or `soroswap` scenarios at
T=1 (single cluster, ~3000/1600 TXs per cluster). Each TX after the first
copies the cached contract code entry bytes into a new CxxBuf allocation.

For `soroswap` T=1 with 1600 TXs and ~118 KB of shared RO data:
- Current: 1599 × ~11 μs = ~18 ms of allocation + memcpy
- Proposed: 1 × ~11 μs (first TX only)
- Savings: ~17 ms / ~713 ms baseline ≈ 2.4%

For `custom_token` T=1 with 3000 TXs and ~47 KB of shared RO data:
- Current: 2999 × ~5 μs = ~15 ms
- Proposed: 1 × ~5 μs
- Savings: ~15 ms / ~640 ms baseline ≈ 2.3%

For T=8 (8 clusters, ~200–375 TXs each), savings scale down proportionally
per thread: ~0.6–1.2% wall-clock improvement.

No impact on SAC scenarios (built-in contract, no large Wasm code entries in
RO footprint).

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:1193-1210` — `serializeLedgerEntryForBridge` copies cached bytes into new CxxBuf per TX
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1212-1231` — `serializeTtlEntryForBridge` same pattern for TTL entries
- `src/transactions/InvokeHostFunctionOpFrame.cpp:471-486` — `addReads` pushes CxxBufs into per-TX vectors
- `src/transactions/InvokeHostFunctionOpFrame.cpp:312-313` — per-TX `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` creation
- `src/transactions/ParallelApplyUtils.h:119-121` — `mRoSerializationCache` definition (would need sibling `mRoLedgerEntryCxxBufs`)
- `src/rust/src/bridge.rs:193-208` — bridge signature (would add separate RO/RW vector params)
- `src/rust/src/soroban_proto_all.rs:101-136` — Rust adaptor (would chain RO+RW iterators)

## Evidence

1. **CxxBufs are borrowed, not consumed**: The bridge signature `ledger_entries: &Vec<CxxBuf>` means Rust borrows the data. The `soroban_proto_all.rs:129` passes `encoded_ledger_entries` as an iterator that borrows CxxBuf elements. After the call, the C++ side still owns the CxxBufs.

2. **Large Wasm entries dominate copy cost**: token.wasm (46 KB), soroswap_router (34 KB), soroswap_pool (27 KB), soroswap_factory (10 KB). These are serialized once per cluster (cache miss) but COPIED per TX (cache hit).

3. **`Chain` iterator preserves `ExactSizeIterator`**: Rust's `Chain<A, B>` implements `ExactSizeIterator` when both sub-iterators do, so `ro_entries.iter().chain(rw_entries.iter())` would satisfy the `e2e_invoke` function's generic `I: ExactSizeIterator` bound.

4. **RO footprint entries are identical across TXs**: In the apply-load benchmark, all TXs in a cluster invoke the same contract(s). The contract code and instance entries in the RO footprint are the same across TXs.

5. **Allocation cost is non-trivial at scale**: For a 46 KB buffer, each alloc+memcpy+dealloc cycle costs ~3–5 μs. Over 3000 TXs, this accumulates to ~10–15 ms.

## Anti-Evidence

1. **Below reliable detection threshold**: At 2–3% improvement, the signal is near the benchmark's ~1–2% noise floor. May require multiple runs to confirm.

2. **Bridge API change complexity**: Adding separate RO/RW vector parameters changes the CXX bridge signature, requiring coordinated changes in C++, Rust bridge, and the adaptor layer. The API surface becomes more complex.

3. **Not all TXs have identical RO footprints**: While the apply-load benchmark uses identical contracts, production workloads may have varied RO footprints per cluster. The optimization would need to detect when RO footprints differ and fall back to per-TX construction.

4. **T=8 improvement is minimal**: With 8 clusters of ~200–375 TXs each, savings per thread are only ~2–4 ms, yielding <1.5% wall-clock improvement.

5. **`rust::Vec<CxxBuf>` lacks `truncate()`/`clear()`**: CXX's `rust::Vec<T>` has limited mutation API. Separating RO/RW into two vectors is cleaner but requires the bridge API change.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4.6, high
**Novelty**: PASS — fail/soroban-env 008+012 targeted caching serialized bytes (avoiding XDR re-serialization, <0.3% savings); this hypothesis proposes eliminating the residual CxxBuf copy entirely via API restructuring, a different mechanism. Fail 024 (SharedPtr) explicitly identifies this API-splitting approach as the viable alternative.

### Trace Summary

Traced the complete path from `InvokeHostFunctionParallelApplyHelper::serializeLedgerEntryForBridge` (line 1193–1210) through the per-TX `mLedgerEntryCxxBufs` vector (line 270, populated at line 485), to the bridge call at line 571 (`invoke_host_function` with `&Vec<CxxBuf>`), through the Rust bridge wrapper (`bridge.rs:193–208`) into `soroban_proto_all.rs:101–136` where entries are passed as `I: ExactSizeIterator<Item = T>`. Confirmed that the Rust side only borrows CxxBuf data via `AsRef<[u8]>` — no consumption or mutation occurs. CxxBufs survive the call intact. Also confirmed `restored_rw_entry_indices` are indices into the RW footprint (not the combined vector), so splitting RO/RW entry vectors would not require index adjustment.

### Code Paths Examined

- `InvokeHostFunctionOpFrame.cpp:1193-1210` — `serializeLedgerEntryForBridge`: cache hit path copies `it->second` (cached `vector<uint8_t>`) into new `CxxBuf` via `make_unique<vector<uint8_t>>(it->second)` — confirmed per-TX heap allocation + memcpy
- `InvokeHostFunctionOpFrame.cpp:270-313` — Per-TX `rust::Vec<CxxBuf>` construction with reserve for full footprint length
- `InvokeHostFunctionOpFrame.cpp:460-492` — `addReads` pushes serialized CxxBufs, called first for RO keys then RW keys (ordering preserved for split)
- `InvokeHostFunctionOpFrame.cpp:525-543` — `addFootprint` calls `addReads(readOnly)` then `addReads(readWrite)` — RO entries always precede RW
- `InvokeHostFunctionOpFrame.cpp:564-573` — Bridge invocation passes `mLedgerEntryCxxBufs` by reference
- `bridge.rs:193-208` — Bridge signature: `ledger_entries: &Vec<CxxBuf>` (Rust borrow, no ownership transfer)
- `soroban_proto_all.rs:101-136` — Adaptor passes entries as generic `I: ExactSizeIterator<Item = T>`, confirming `Chain` would satisfy the bound
- `ParallelApplyUtils.h:114-122` — `mRoSerializationCache` definition at thread level; sibling `rust::Vec<CxxBuf>` for persistent RO CxxBufs would be stored here
- `InvokeHostFunctionOpFrame.cpp:1212-1220` — TTL entries are NOT cached (correct: values change within cluster via `flushRoTTLBumpsInTxWriteFootprint`)

### Findings

The inefficiency is confirmed: on each cache hit for RO entries, `serializeLedgerEntryForBridge` heap-allocates a new `vector<uint8_t>` and memcpy's the cached bytes into it (line 1206-1207). For large Wasm code entries (46 KB token, 118 KB soroswap total), this costs ~3–14 μs per TX in allocation + memcpy + deallocation overhead. Over 1600–3000 TXs in a T=1 cluster, aggregate cost is ~15–18 ms against a 640–713 ms baseline (2–3%).

The proposed fix (split bridge API into separate RO/RW entry vectors, store RO CxxBufs at thread level) is architecturally sound:
- Rust's `Chain<A, B>` preserves `ExactSizeIterator` when both sub-iterators implement it
- `restored_rw_entry_indices` are RW-footprint-relative indices, unaffected by the split
- TTL entries would need separate RO/RW vectors too (RO TTLs change within cluster, so RO TTL CxxBufs cannot be reused — but RO ledger entry CxxBufs can be)

**Severity downgrade from Low to Informational**: The estimated 2–3% improvement for T=1 scenarios falls below the 5% Low threshold. For T=8 (more realistic production scenario), improvement drops to <1.5%, overlapping with the benchmark's ~1–2% noise floor. The bridge API change (C++ bridge definition + Rust wrapper + Rust adaptor) adds complexity disproportionate to the marginal gain.

### PoC Guidance

- **Target code**: (1) `src/transactions/ParallelApplyUtils.h` — add `mutable rust::Vec<CxxBuf> mRoLedgerEntryCxxBufs` and `mutable rust::Vec<CxxBuf> mRoTtlEntryCxxBufs` to `ThreadParallelApplyLedgerState`; (2) `src/transactions/InvokeHostFunctionOpFrame.cpp:1193-1210` — on cache hit, skip CxxBuf creation, record that entry is in the thread-level RO vector; (3) `src/rust/src/bridge.rs:193-208` — add `ro_ledger_entries: &Vec<CxxBuf>`, `ro_ttl_entries: &Vec<CxxBuf>` parameters; (4) `src/rust/src/soroban_proto_all.rs:101-136` — chain `ro_entries.iter().chain(rw_entries.iter())` for both ledger and TTL entries
- **Change description**: Split bridge entry parameters into RO (thread-level, built once per cluster) and RW (per-TX). First TX in cluster fills the RO CxxBuf vectors via `serializeLedgerEntryForBridge`; subsequent TXs pass the same RO vectors by reference
- **Correctness check**: `[soroban]` and `[tx]` test suites cover invoke host function paths; parallel apply tests in `ParallelSorobanApplyTests` validate cluster semantics
- **Benchmark focus**: `custom_token` T=1 and `soroswap` T=1 should show ~2–3% improvement in median close time. T=8 improvement will likely be below noise. SAC scenarios should be unaffected (no large Wasm RO entries)
