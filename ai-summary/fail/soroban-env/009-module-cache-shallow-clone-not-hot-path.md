# H009: Module-Cache `shallow_clone()` Handle Churn Is Not a Hot-Path Bottleneck

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: Module-cache handle setup
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If module-cache handle cloning were a meaningful optimization target for
apply-load, it would need to happen on or near the per-transaction bridge path.
An expensive clone on every Soroban invocation could plausibly serialize
parallel apply and reduce T=8 scaling.

## Mechanism

The suspected mechanism was that `LedgerManagerImpl::getModuleCache()` returns a
`shallow_clone()` and that each apply thread then carries its own
`SorobanModuleCache`, potentially paying repeated clone/setup overhead. If that
clone happened per transaction, eliminating it could reduce bridge setup and
cross-thread cache-handle churn.

## Trigger

Run `soroswap,T=8` and inspect module-cache handle creation while the parallel
apply stage launches worker threads.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:940-946` — `getModuleCache()` returns `shallow_clone()`
- `src/ledger/LedgerManagerImpl.cpp:2441-2449` — thread states are created once per cluster in a stage
- `src/transactions/ParallelApplyUtils.cpp:610-618` — each `ThreadParallelApplyLedgerState` captures one cloned module-cache handle
- `src/rust/src/soroban_module_cache.rs:54-60` — top-level `SorobanModuleCache::shallow_clone()`
- `src/rust/src/soroban_proto_any.rs:787-795` — protocol-specific clone resets local counters/cache slots but clones the underlying module-cache handle

## Evidence

The clone is real: `LedgerManagerImpl::getModuleCache()` returns a shallow
clone, and `ThreadParallelApplyLedgerState` stores one such handle. The Rust
side also reinitializes a few wrapper fields when cloning.

## Anti-Evidence

Thread-state creation happens once per cluster per stage, not once per
transaction. The clone itself is just a shallow handle copy around the
thread-safe underlying module cache, so even in T=8 it occurs only a handful of
times per ledger close, far away from the dominant per-TX bridge marshaling
path.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The frequency is wrong. The measured path clones module-cache handles when
thread states are constructed for a stage, while the expensive bridge work we
care about happens for every Soroban transaction inside those already-created
threads. Even a perfect elimination of `shallow_clone()` would only save a few
handle copies per ledger close.

### Lesson Learned

For this objective, bridge optimizations need to hit the per-transaction or
per-reused-entry paths. Per-stage setup work that happens once per cluster is
too infrequent to move the apply-load benchmark unless it hides a much larger
downstream cost.
