# H002: Cache Shared Read-Only Footprint CxxBufs Across Invoke-Host Transactions

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: High
**Impact**: C++↔Rust bridge marshalling overhead in Soroban apply
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Repeated reads of the same immutable read-only footprint entries within a ledger
close should reuse previously serialized `CxxBuf` payloads instead of
reloading and re-encoding the same `LedgerEntry` and `TTLEntry` bytes for every
transaction.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` serializes every footprint entry and
TTL with fresh `toCxxBuf` calls on every invoke-host tx. In apply-load, many
read-only keys are shared across the entire workload: SAC reuses one instance
entry, custom-token reuses the same contract-code plus instance entries, and
soroswap reuses router/pair code plus shared instance entries. Contract-code
entries are especially expensive because the serialized `LedgerEntry` includes
the full Wasm blob. A per-thread or per-stage cache keyed by `(LedgerKey,
liveUntilLedgerSeq)` should eliminate thousands of identical XDR serializations
and heap allocations per ledger.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile
`InvokeHostFunctionApplyHelper::addReads` / `toCxxBuf`. Compare current behavior
against a build that memoizes serialized read-only entry+TTL buffers for the
duration of a ledger or cluster. The strongest signal should be in
`custom_token,TX=1600,T=1|8` and `soroswap,TX=1000,T=1|8`, which repeatedly
ship contract-code entries across the bridge.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-466` — `addReads` reloads and serializes every footprint entry / TTL every tx
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` always allocates and serializes a fresh byte vector
- `src/simulation/ApplyLoad.cpp:1150-1153` — SAC benchmark reuses the same instance key as read-only input
- `src/simulation/ApplyLoad.cpp:2207-2211` and `src/simulation/TxGenerator.cpp:840-845` — custom-token transfers reuse one contract-code key and one instance key in every tx
- `src/simulation/ApplyLoad.cpp:3140-3149` — soroswap swaps repeatedly ship router/pair code and shared instance keys as read-only inputs

## Evidence

- `addReads` does not have any cache, arena, or reuse path; every successful
  read goes through `toCxxBuf(*entryOpt)` and `toCxxBuf(*ttlEntry)`.
- Apply-load explicitly seeds shared read-only keys at scenario setup time:
  one SAC instance for XLM transfers, one contract-code + instance pair for the
  token workload, and shared router/pair code keys for soroswap.
- Contract-code keys are carried in the read-only footprint for
  `custom_token` and `soroswap`, meaning the bridge is repeatedly serializing
  large immutable Wasm-containing ledger entries, not just small account data.

## Anti-Evidence

- Read-write entries still need per-tx serialization, so the optimization only
  attacks the shared read-only subset.
- Read-only TTLs can change across ledgers (and in some cases via bumps), so
  the cache key must include the live-until value or be scoped tightly enough
  to avoid stale reuse.
- Soroswap’s token pair selection varies, so not every tx reuses the exact same
  full read-only footprint.
