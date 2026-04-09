# H002: Repeated RO footprint marshaling wastes Soroban apply CPU

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Medium
**Impact**: C++<->Rust bridge CPU / allocation churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When many transactions in the same cluster touch the same immutable read-only contract entries, apply should marshal those entries to Rust once per cluster or stage and reuse the serialized buffers. The hot loop should not repeatedly XDR-encode identical contract code, contract instance, and TTL entries for every transaction.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` loads each footprint key, converts the ledger entry and TTL entry to fresh `CxxBuf` objects with `toCxxBuf`, and appends them to per-tx vectors. The apply-load scenarios intentionally reuse the same RO objects across many txs: SAC benchmarks reuse the same SAC and batch-transfer instances, and Soroswap benchmarks reuse the router instance/code and pair code. Without a per-thread cache keyed by `LedgerKey` plus entry version/TTL, the bridge does repeated snapshot lookups, XDR serialization, and heap allocation for identical RO data on every host call.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile `soroswap,TX=1600,T=8` or `sac,TX=6400,T=8`. Expect visible CPU and allocation activity in `toCxxBuf`, XDR marshaling, and `std::vector<uint8_t>` construction beneath `InvokeHostFunctionApplyHelper::addReads`.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-503` - reloads and serializes every footprint key for every tx
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addFootprint/invokeHostFunction:507-553` - feeds the freshly marshaled vectors into the Rust bridge
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2073-2111` - all txs in a cluster reuse the same batch-transfer and SAC contract IDs
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSoroswapSwaps:3140-3168` - every swap reuses router/code and pair-code RO keys

## Evidence

The RO-path marshaling code is plainly per-tx: `addReads` iterates the RO footprint, loads each live entry, calls `toCxxBuf(*entryOpt)` and `toCxxBuf(*ttlEntry)`, and pushes both into tx-local `rust::Vec<CxxBuf>` buffers. The Soroswap generator explicitly places router instance, two SAC instances, router code, and pair code in the RO footprint for every generated swap, making repeated serialization unavoidable in the current design.

## Anti-Evidence

RW entries cannot be safely shared the same way because later txs in the same cluster may observe prior writes, and archived-entry handling can also mutate restore bookkeeping. Any cache has to be limited to RO entries whose serialized form is stable for the lifetime of the thread or stage.
