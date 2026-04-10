# H004: Use Known Resource Budgets To Pre-Reserve Batch Buffer Bytes

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Low
**Impact**: allocator and memcpy overhead during per-tx batch assembly
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

`CxxBatchBufBuilder` should write ledger-entry, TTL, and auth batches into
buffers whose byte capacity is already close to the final serialized size.
Large Soroban transactions should not repeatedly reallocate and copy partially
built batch payloads while the apply path is assembling bridge inputs.

## Mechanism

The builder already exposes `reserve(numEntries, estimatedTotalBytes)`, but all
current callers pass only the entry count. As a result, `append()` grows
`mData` incrementally with `resize(offset + sz)` for every entry, so large
footprints can trigger several reallocations and memcpys of already-serialized
bytes. The operation already knows a tight byte budget for the read set
(`mResources.diskReadBytes`) and the number of TTL/auth entries, so it can
pre-reserve most of the final buffer size before serialization begins.

## Trigger

Run `custom_token` or `soroswap` apply-load, especially `T=8`, and sample
allocator / memcpy hotspots under `CxxBatchBufBuilder::append`. Compare against
a build that reserves `mLedgerEntryBatch` and `mTtlEntryBatch` using known
resource-byte bounds and cached auth lengths before the append loop starts.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:45-58` — builder already supports `estimatedTotalBytes`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:64-73` — `append()` resizes the contiguous buffer for each entry
- `src/transactions/InvokeHostFunctionOpFrame.cpp:366-367` — ledger and TTL batches reserve only entry counts
- `src/transactions/InvokeHostFunctionOpFrame.cpp:507-518` — large live-entry / TTL payloads are appended in the hot path
- `src/transactions/InvokeHostFunctionOpFrame.cpp:582-587` — auth batch likewise reserves only count, not bytes

## Evidence

The builder API already contains the exact hook needed for byte reservation, but
no caller uses it. The apply path also already has resource-byte information
attached to the operation, so this is not speculative metadata that would need
to be computed from scratch.

## Anti-Evidence

`std::vector` growth is geometric, so the number of reallocations per tx is
bounded rather than linear in entry count. Small-footprint SAC workloads may
see little or no improvement; this should primarily affect the larger
`custom_token` / `soroswap` payloads.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced `CxxBatchBufBuilder` usage through `InvokeHostFunctionApplyHelper` constructor (lines 365-367) and the `addReads` loop (lines 414-557). Confirmed that `reserve(footprintLength)` only pre-sizes `mLengths`, leaving `mData` at zero capacity. Each `append()` call invokes `mData.resize(offset + sz)`, which triggers geometric reallocation from zero. The `SorobanResources::diskReadBytes` field is available on `mResources` and provides an upper-bound byte estimate for the ledger entry batch. The fix is trivially correct — pass `diskReadBytes` as the second argument to `reserve()`. TTL entries are small (~50-100 bytes each) and can be estimated as `footprintLength * 100`.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:45-93` — `CxxBatchBufBuilder` class: `reserve()` accepts `estimatedTotalBytes` but callers never use it; `append()` calls `mData.resize(offset + sz)` growing from zero capacity
- `src/transactions/InvokeHostFunctionOpFrame.cpp:365-367` — Constructor reserves entry count only: `mLedgerEntryBatch.reserve(footprintLength)` with no byte estimate
- `src/transactions/InvokeHostFunctionOpFrame.cpp:502-519` — `addReads()` appends ledger entries and TTL entries per-footprint-key in the hot path
- `src/transactions/InvokeHostFunctionOpFrame.cpp:320` — `mResources` is `SorobanResources const&` with `diskReadBytes` field available at construction time
- `src/transactions/InvokeHostFunctionOpFrame.cpp:582-587` — Auth batch built in `invokeHostFunction()` with count-only reserve; auth entries are typically 1-5 and small, minimal reallocation overhead

### Findings

**The inefficiency is real but small.** For a soroswap transaction with ~30 footprint entries totaling ~120KB of serialized ledger data, `mData` grows geometrically from 0 through ~17 doublings. This involves ~17 malloc/free pairs (~3µs allocator overhead) and ~240KB total memcpy of already-serialized data (~12µs at ~20GB/s). Total per-tx reallocation overhead: ~15µs.

For soroswap at TX=1000, aggregate: ~15ms against ~2-5s ledger close = **0.3-0.75%**. For custom_token at TX=1600 with smaller footprints (~50KB), aggregate: ~8ms = **0.2-0.4%**. For SAC at TX=3200 with tiny footprints (~5-10KB), the overhead is negligible (<0.1%).

**The fix is trivially correct**: change line 366 to `mLedgerEntryBatch.reserve(footprintLength, mResources.diskReadBytes)` and line 367 to `mTtlEntryBatch.reserve(footprintLength, footprintLength * 100)`. No correctness constraints are violated — `diskReadBytes` is a declared upper bound that the operation already checks against, and over-reservation only wastes temporary capacity that is freed when `build()` moves the vector.

**Severity downgraded to Informational.** The improvement ceiling is ~0.3-0.75% of ledger close time for the heaviest scenario, well below the 5% threshold for Low severity. The optimization is real and the code change is trivial, but should not be expected to produce a measurable benchmark delta.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` lines 365-367 (constructor) and optionally line 583 (auth batch)
- **Change description**: Pass `mResources.diskReadBytes` as `estimatedTotalBytes` to `mLedgerEntryBatch.reserve()`, and `footprintLength * 100` to `mTtlEntryBatch.reserve()`. For auth batch, optionally compute total auth size with a pre-pass `xdr_size` sum.
- **Correctness check**: All existing InvokeHostFunction tests cover this code path; the change only affects buffer pre-allocation, not serialization content.
- **Benchmark focus**: soroswap T=8 TX=1000 median ledger close time; expected improvement ≤1%. Check allocator sampling (e.g., Tracy or perf) for reduced malloc count in `CxxBatchBufBuilder::append` rather than relying on wall-clock delta.
