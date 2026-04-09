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
