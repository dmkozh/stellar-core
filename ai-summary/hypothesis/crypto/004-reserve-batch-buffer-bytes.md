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
