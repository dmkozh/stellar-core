# H003: Cache Ledger-Key XDR Sizes for Large Soroban Footprints

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Medium
**Impact**: bridge-side metering CPU in addReads / recordStorageChanges
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

For a Soroban transaction, ledger-key byte sizes used in resource metering
should be computed once per unique key and then reused throughout the invoke
path. The apply path should not repeatedly traverse the same large
`LedgerKey` objects with `xdr::xdr_size` during both footprint ingestion and
host-result processing.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` computes `xdr::xdr_size(lk)` for every
footprint key before host execution, and `recordStorageChanges` computes
`xdr::xdr_size(lk)` again for every returned write. Batched SAC transactions are
especially pathological: each tx has 100 destination balance keys plus the
batch-transfer source balance, and the writeback path revisits those same
`CONTRACT_DATA` keys and their TTLs. Because those keys embed nested SCVals
(`["Balance", Address]`), repeated `xdr_size` walks become a pure C++ hot-path
tax that scales with batch size rather than host semantics.

## Trigger

Run the apply-load matrix on batched SAC (`sac,TX=6400,T=8`, default
`APPLY_LOAD_BATCH_SAC_COUNT=100`) and profile
`InvokeHostFunctionApplyHelper::addReads` plus `recordStorageChanges`. The hot
path should show heavy time in `xdr::xdr_size` on `LedgerKey` objects even
though the key shapes are immutable within each tx.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-503` — computes `keySize` with `xdr::xdr_size(lk)` for every footprint key
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::recordStorageChanges:610-703` — recomputes `xdr::xdr_size(lk)` for every returned modified entry
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionParallelApplyHelper::handleArchivedEntry:1027-1054` — repeats key-size work for restored RW entries
- `src/simulation/TxGenerator.cpp:TxGenerator::invokeBatchTransfer:1480-1515` — creates 100+ large balance keys per tx in the benchmark's hottest SAC mode

## Evidence

The current code uses `xdr::xdr_size(lk)` in three separate invoke-host
subpaths, but there is no helper-local cache of key sizes or even a
per-footprint vector of precomputed sizes. Batched SAC magnifies this because a
single tx constructs 100 destination `Balance` keys in the RW footprint, and
the writeback path revisits the same key shapes after host execution, turning
one logical key set into hundreds of recursive size walks per tx.

## Anti-Evidence

`xdr::xdr_size` does not allocate, so the per-call cost is lower than the
serialization-heavy bridge work already identified elsewhere. The win is also
highly workload-dependent: custom-token and Soroswap transfers have much
smaller footprints, so this optimization is likely strongest on batched SAC and
weaker on the other matrix scenarios.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced `xdr::xdr_size` through the xdrpp template hierarchy (`types.h:224-227`
→ `serial_size` dispatch via `xdr_struct_base_vs`, `xdr_container_base`,
`field_size_t`). For `LedgerKey` with `CONTRACT_DATA` containing nested SCVal,
the recursive walk visits ~10-15 nodes doing pure integer addition — no heap
allocations, no copies, no I/O. At protocol 23+, the `keySize` computed in
`addReads` (line 372) is **dead code** for Soroban entries: the only consumer
`meterDiskReadResource` (line 491) is gated by
`!isSorobanEntry(lk) || protocolVersionIsBefore(...)`, so it's skipped for all
Soroban keys on the parallel apply path.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:addReads:369-503` — `keySize` computed at line 372 via `xdr::xdr_size(lk)`, but at p23+ for Soroban entries, the only consumer `meterDiskReadResource` (line 487-495) is skipped. The value is dead.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:recordStorageChanges:610-703` — `keySize` computed at line 632, consumed by `noteWriteEntry` (line 639) for write byte metrics. This computation is needed and non-redundant since the LedgerKey is reconstructed from host output (`LedgerEntryKey(le)` at line 620), not from footprint.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:handleArchivedEntry:1016-1090` — `keySize` computed at line 1029, consumed by `meterDiskReadResource` (line 1054). Only fires for archived entries, which is a rare/empty set in the benchmark's hot path.
- `lib/xdrpp/xdrpp/types.h:224-227` — `xdr_size<T>` dispatches to `xdr_traits<T>::serial_size(t)`, a compile-time-generated recursive walk over struct/union fields doing integer addition only.
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` calls `xdr::xdr_to_opaque(t)` which does heap allocation (`make_unique<vector<uint8_t>>`) plus full XDR serialization. Called on the same path for every entry, dwarfing `xdr_size` cost.

### Why It Failed

Three independent reasons:

1. **Dead code at p23+**: In `addReads`, `keySize` is computed for every Soroban footprint key but never consumed at p23+ (the `meterDiskReadResource` call is gated off). The "redundancy" between `addReads` and `recordStorageChanges` is illusory — the first computation's result isn't used.

2. **Negligible cost**: `xdr::xdr_size` is pure integer arithmetic inlined by the compiler. For a CONTRACT_DATA key with nested SCVal, it visits ~10-15 template-dispatched nodes. At ~200 keys per batched SAC tx, this totals ~3000 integer additions — roughly 1-3 microseconds. Compare to `toCxxBuf` on the same path: heap allocation + full XDR serialization per entry, which is 10-100x more expensive per call.

3. **Cache overhead rivals saved computation**: A cache would require either (a) an `UnorderedMap<LedgerKey, uint32_t>` where hashing a LedgerKey involves traversing the same structure as `xdr_size`, or (b) index-based lookup which doesn't work because `recordStorageChanges` reconstructs keys from host output in arbitrary order. Either approach adds overhead comparable to the computation being saved.

### Lesson Learned

`xdr::xdr_size` is a zero-allocation recursive template walk — essentially free compared to serialization (`xdr_to_opaque`), heap allocation (`toCxxBuf`), and FFI calls on the same path. When evaluating XDR-related overhead, distinguish between size computation (cheap) and serialization (expensive). Also verify that computed values are actually consumed at the target protocol version before claiming redundancy.
