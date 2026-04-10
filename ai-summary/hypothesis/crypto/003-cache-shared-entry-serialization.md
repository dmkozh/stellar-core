# H003: Cache Serialized Shared Footprint Entries Across Transactions In A Ledger Close

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Medium
**Impact**: repeated XDR size-pass, allocation, and serialization of identical live entries before FFI
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If many Soroban transactions in the same ledger close read the same immutable
ledger entries — contract code, contract instances, router/factory state, pair
code, shared balance objects — stellar-core should serialize those shared
entries once and reuse the encoded bytes until the entry becomes dirty. The hot
apply path should not re-run `xdr_to_opaque` for the exact same live entry every
time another transaction references it.

## Mechanism

Parallel apply already shares live entry objects aggressively: a thread state
preloads entries from `mGlobalEntryMap`, and misses then read from the
in-memory Soroban state or live snapshot. But `InvokeHostFunctionApplyHelper`
throws away that sharing right before the bridge by calling `toCxxBuf(*entryOpt)`
for every footprint read on every transaction. In the apply-load workloads, many
transactions reuse the same read-only entries repeatedly (for example the same
router instance, router code, pair code, token SAC instances, or token contract
code). A per-thread or global cache of serialized bytes for clean shared entries
— invalidated when the corresponding entry becomes dirty in the thread/global
maps — would eliminate repeated XDR size-pass + allocation + serialization work
for those reused live entries.

## Trigger

Run `custom_token` or `soroswap` apply-load and profile `xdr::xdr_to_opaque`
inside `InvokeHostFunctionApplyHelper::addReads`. Look specifically for the same
router/code/instance entries being serialized across many transactions in the
same ledger close. Compare against a build that caches encoded bytes for clean
global/thread entries and reuses them until the entry is dirtied.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:563-608` — thread state preloads shared footprint entries from the global map for the whole cluster
- `src/transactions/ParallelApplyUtils.cpp:699-735` — later reads fall back to shared thread/global/snapshot entries
- `src/transactions/InvokeHostFunctionOpFrame.cpp:369-467` — `addReads` reserializes every loaded entry into fresh bridge buffers for each tx
- `src/transactions/TransactionUtils.h:370-376` — `toCxxBuf` always performs a fresh `xdr::xdr_to_opaque`
- `src/simulation/ApplyLoad.cpp:2252-2277` — `custom_token` transactions all reuse the same token code/instance entries while varying only account balance keys
- `src/simulation/ApplyLoad.cpp:3140-3168` — every `soroswap` swap reuses the same router instance, router code, pair code, and SAC read-only entries across many txs

## Evidence

The data-sharing layer is already present: global and thread state keep common
entries alive across transactions, and the benchmark workloads explicitly build
many transactions with repeated read-only keys. What is missing is only the
serialized form: the bridge preparation step ignores that shared object graph
and recomputes identical XDR buffers every time a transaction touches a shared
entry.

## Anti-Evidence

Write-set entries that change every transaction cannot safely reuse cached bytes
after they become dirty, so the benefit is concentrated in shared clean entries.
The cache therefore needs precise invalidation or “clean-only” scoping to avoid
serving stale serialized bytes.
