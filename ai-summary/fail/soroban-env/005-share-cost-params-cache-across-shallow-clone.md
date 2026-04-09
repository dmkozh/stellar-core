# H005: Share Cached ContractCostParams Across SorobanModuleCache::shallow_clone via Arc

**Date**: 2025-07-22
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: Eliminate per-thread first-TX cost params deserialization in parallel apply
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `SorobanModuleCache::shallow_clone()` creates per-thread copies for
parallel apply, the cached `ContractCostParams` (populated by the first TX
on the main thread or a previous ledger close) should be shared across all
thread copies. Each thread should not need to independently deserialize and
cache the cost params on its first TX.

## Mechanism

`ProtocolSpecificModuleCache::shallow_clone()` (soroban_proto_any.rs:711-720)
creates a new cache with `cached_cpu_cost_params: RwLock::new(None)` and
`cached_mem_cost_params: RwLock::new(None)`, discarding any previously cached
cost params. This means each of the 8 parallel-apply threads must
independently deserialize the cost params on its first TX invocation.

If the `RwLock<Option<(Vec<u8>, ContractCostParams)>>` were replaced with
an `Arc<RwLock<...>>` or the cached values were cloned into the new cache,
each thread would start with a warm cache.

The saving is ~3-8μs per thread (one XDR deserialization of ~300-600 bytes ×
2 params), amortized across 8 threads. Total saving: ~24-64μs per ledger
close. Against ~700ms T=8 baseline: ~0.003-0.009%. This is completely
negligible — a single cache miss on the first of ~800 TXs per thread.

## Trigger

Run apply-load at T=8. Each thread's first TX pays the cost params
deserialization penalty.

## Target Code

- `src/rust/src/soroban_proto_any.rs:711-720` — `ProtocolSpecificModuleCache::shallow_clone()`
- `src/rust/src/soroban_proto_any.rs:797-831` — cost params cache lookup logic
- `src/rust/src/soroban_module_cache.rs:54-61` — `SorobanModuleCache::shallow_clone()`

## Evidence

- `shallow_clone()` explicitly resets cache to `None` on lines 715-716
- Each thread independently pays deserialization cost on first TX (soroban_proto_any.rs:808-830)
- The cost params are identical across all threads within a ledger close

## Anti-Evidence

- The one-time-per-thread deserialization cost is amortized over ~200-800 TXs per thread
- Net saving is ~24-64μs per ledger close — orders of magnitude below benchmark noise
- Using Arc<RwLock<...>> adds complexity (cross-thread synchronization) for negligible gain
- The RwLock::new(None) reset is intentional defensive design — fresh state per thread

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (fail #003 investigated CxxLedgerInfo heap allocs, not cache sharing across clones)

### Why It Failed

The saving is ~24-64μs per ledger close (~0.003-0.009% of benchmark time).
This is 3-4 orders of magnitude below the benchmark noise floor. The
optimization would add cross-thread synchronization complexity (Arc<RwLock>)
for a completely unmeasurable performance difference.

### Lesson Learned

One-time-per-thread initialization costs amortized over hundreds of TXs are
not viable optimization targets. The cost params cache in success #001 was
impactful because it eliminated PER-TX deserialization (~38μs × thousands of
TXs), not because the single deserialization was expensive.
