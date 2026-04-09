# H002: Module Cache Getter Cloning Is the Main T=8 Apply Bottleneck

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Low
**Impact**: Parallelization / CPU
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Parallel Soroban apply should not spend a meaningful fraction of ledger-close
time cloning bridge wrappers around the shared module cache. The module-cache
handle should be obtained infrequently enough that it does not materially limit
throughput.

## Mechanism

The suspected issue was that `app.getModuleCache()` might be cloning expensive
state for every parallel Soroban transaction or every worker-thread transition,
creating enough allocator and lock traffic to blunt the `T=8` benchmark. That
would have been especially concerning if each clone duplicated the full Rust-side
module cache contents.

## Trigger

Profile the `T=8` apply-load scenarios and attribute time to
`LedgerManagerImpl::getModuleCache`, `SorobanModuleCache::shallow_clone`, and
`ThreadParallelApplyLedgerState` construction.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::getModuleCache:939-947` — apply-time getter
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::ThreadParallelApplyLedgerState:610-623` — per-thread acquisition of the module cache handle
- `src/rust/src/soroban_module_cache.rs:SorobanModuleCache::shallow_clone:54-61` — clone implementation
- `src/rust/src/soroban_proto_any.rs:ProtocolSpecificModuleCache::shallow_clone:787-795` — underlying protocol-cache clone behavior

## Evidence

Each parallel thread state does fetch a module-cache handle, and the Rust side
creates wrapper objects during `shallow_clone`. This looked like a potential
serial setup cost in the multi-threaded apply path.

## Anti-Evidence

The clone is shallow and shares the underlying threadsafe `ModuleCache`; it does
not duplicate compiled modules. In the benchmarked p26 path the getter runs once
per thread-state creation, not once per host call, and recent history includes
`e6638811e Fix expensive module cache getter`, suggesting this exact angle has
already been reduced.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The current getter only creates shallow shared handles, so it is not the kind of
per-transaction heavyweight clone needed to explain a major apply-load slowdown.

### Lesson Learned

Differentiate wrapper-handle churn from actual cache duplication: in this bridge,
`shallow_clone` is intentionally cheap and shares the real module cache.
