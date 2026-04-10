# H004: Host writeback pays a full hash-set diff to prove that no benchmark keys were deleted

**Date**: 2026-04-10
**Subsystem**: transactions
**Severity**: Low
**Impact**: writeback hash churn / redundant full-footprint scan
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The bridge output should describe deletions directly, so the C++ writeback path only processes keys that were actually removed. It should not have to hash every returned entry and then rescan the full read-write footprint to infer an empty deletion set for transfer/swap workloads that only create or update entries.

## Mechanism

`InvokeHostFunctionOutput` only returns surviving `modified_ledger_entries`, so `recordStorageChanges` reconstructs a `createdAndModifiedKeys` set, tracks `createdKeys`, and then scans every RW footprint key to discover which ones were omitted and therefore deleted. The apply-load model transactions do not exercise entry-deletion-heavy contracts — they transfer balances, update trustlines, and mutate pool state — so this set-diff is usually proving "nothing deleted" after paying for two hash sets and a second full RW-footprint pass.

## Trigger

Run `scripts/run_apply_load_matrix.py` on `sac,TX=6400,T=8` or `soroswap,TX=1600,T=8` and profile `InvokeHostFunctionApplyHelper::recordStorageChanges`. Expect time in `createdAndModifiedKeys.insert`, `createdKeys.insert`, and the final `for (lk : readWrite)` scan even when the model contracts only create/update balances and pair state.

## Target Code

- `src/rust/src/bridge.rs:InvokeHostFunctionOutput:30-55` — bridge output has no explicit deleted-entry list
- `src/rust/src/soroban_proto_any.rs:304-310` — omitted ledger entries are interpreted as deletions
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::recordStorageChanges:614-702` — builds two hash sets and rescans the RW footprint to infer deletions
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2105-2148` — batch-transfer benchmark creates or updates balances, then prevalidates
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateTokenTransfers:2323-2342` — token benchmark is pure transfer/update workload
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3140-3168` — Soroswap benchmark updates trustlines, balances, and pair state, not delete-heavy paths

## Evidence

The Rust-side contract invocation comment states that "ledger entries not returned have been deleted," and the C++ side implements exactly that inference with `createdAndModifiedKeys` plus a second scan over `mResources.footprint.readWrite`. The benchmark generators target transfer/swap workloads where deletions are atypical, so the generic deletion-recovery protocol spends apply time on a negative proof nearly every transaction.

## Anti-Evidence

The current interface is simple and correct for arbitrary contracts, and some real contracts can delete entries, so the optimization likely requires a bridge-format extension such as deleted RW indices or an explicit deletion vector. If the extra output shape complicates host compatibility too much, the practical win may end up limited to benchmark-oriented builds.
