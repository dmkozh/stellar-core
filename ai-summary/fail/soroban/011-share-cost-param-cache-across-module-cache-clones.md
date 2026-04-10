# H011: Share Cost-Param Cache Across Shallow-Cloned Module Cache Wrappers

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: Rust bridge CPU / repeated cost-param deserialization
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The p23+ cost-parameter cache should survive `SorobanModuleCache::shallow_clone`
so that all apply-path users of the shared module cache can reuse the first
parsed `ContractCostParams`. Parallel worker threads and pre-v23 apply helpers
should not need to deserialize the same cost-parameter XDR again just because
they received a cloned wrapper.

## Mechanism

`AppConnector::getModuleCache()` returns a shallow clone of the reusable module
cache, and `ProtocolSpecificModuleCache::shallow_clone()` reinitializes
`cached_cpu_cost_params` and `cached_mem_cost_params` to `None`. That means the
first host invocation in each clone misses the Rust-side cache and redoes the
`ContractCostParams` XDR decode, even though the underlying `ModuleCache` is
shared and the serialized cost-param bytes are identical for the whole ledger.

## Trigger

Profile the first Soroban invocation on each parallel worker thread in a p26
apply-load run, or the first invoke in each pre-v23 helper path. Look for extra
`get_or_deserialize_cost_params` misses that occur once per shallow clone rather
than once per ledger.

## Target Code

- `src/main/AppConnector.cpp:AppConnector::getModuleCache:127-130` — obtains a shallow-cloned module-cache handle
- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::getModuleCache:939-947` — returns `mApplyState.getModuleCache()->shallow_clone()`
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState:610-618` — acquires one cloned handle per worker thread
- `src/rust/src/soroban_module_cache.rs:SorobanModuleCache::shallow_clone:54-60` — clones wrapper caches per protocol
- `src/rust/src/soroban_proto_any.rs:ProtocolSpecificModuleCache::shallow_clone:787-795` — resets cost-param caches to `None`
- `src/rust/src/soroban_proto_any.rs:ProtocolSpecificModuleCache::get_or_deserialize_cost_params:797-817` — cache miss path redoes XDR decode

## Evidence

The cost-param cache is stored inside each `ProtocolSpecificModuleCache`, not in
the shared underlying `ModuleCache`. Because `shallow_clone()` creates fresh
`RwLock<Option<...>>` fields, the first invocation in every cloned wrapper
necessarily misses and reparses the same cost params.

## Anti-Evidence

Each clone only pays this miss once. In the benchmarked p26 parallel path that
usually means one extra deserialize per worker thread per stage, not per
transaction.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The inefficiency is real but far too small. Even with 8 worker threads, this
only adds eight extra parses of two ~1.7KB `ContractCostParams` blobs at the
start of a stage, which is on the order of tens of microseconds per ledger — far
below a measurable threshold in apply-load.

### Lesson Learned

Be careful to distinguish cache invalidation that happens once per thread or
stage from costs that happen once per transaction. The shallow-clone cache reset
breaks perfect amortization, but its frequency is too low to matter in the
benchmark.
