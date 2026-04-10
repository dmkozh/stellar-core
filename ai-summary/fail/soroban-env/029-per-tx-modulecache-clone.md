# H029: Per-TX `ModuleCache::clone()` in `invoke_host_function_with_trace_hook_and_module_cache`

**Date**: 2025-07-18
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / redundant reference counting
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The `ModuleCache` should be passed by reference into `e2e_invoke::invoke_host_function`
rather than cloned on every TX invocation, avoiding redundant Arc reference
count increments and decrements.

## Mechanism

In `soroban_proto_all.rs:134`, every call to
`invoke_host_function_with_trace_hook_and_module_cache` clones the
`ModuleCache` via `module_cache.p26_cache.module_cache.clone()`. The
`ModuleCache` struct (from soroban-env-host's `vm/module_cache.rs`) uses
`#[derive(Clone)]` and contains:

- `wasmi_engine: wasmi::Engine` — likely Arc-backed (shallow clone)
- `wasmi_linker: wasmi::Linker<Host>` — likely Arc-backed (shallow clone)
- `modules: ModuleCacheMap(Arc<Mutex<BTreeMap<...>>>)` — Arc increment

The module_cache.rs source comment explicitly states: "The cache can be
cloned, but the clone is a shallow copy." Each clone involves 2-3 atomic
reference count operations (~5-10 ns each), totaling ~15-30 ns per TX.

## Trigger

Run SAC @ 3200 TXs: 3200 × 25 ns = ~80 μs per ledger. At baseline ~2500 ms
(T=1): 0.003%. At T=8 ~350 ms: 0.023%.

## Target Code

- `src/rust/src/soroban_proto_all.rs:134` — `Some(module_cache.p26_cache.module_cache.clone())`
- `src/rust/soroban/p26/soroban-env-host/src/vm/module_cache.rs:20-27` — `ModuleCache` struct with `#[derive(Clone)]`
- `src/rust/soroban/p26/soroban-env-host/src/vm/module_cache.rs:45` — `ModuleCacheMap(Arc<Mutex<...>>)` confirms shallow clone

## Evidence

The clone happens on EVERY TX invocation (not per-cluster like F001's
`shallow_clone()`, which was about `ProtocolSpecificModuleCache`). The
`e2e_invoke::invoke_host_function` takes `Option<ModuleCache>` by value,
requiring the caller to clone. Passing by reference would eliminate this cost.

## Anti-Evidence

1. The clone is explicitly designed to be shallow (~15-30 ns). The module_cache.rs
   comment confirms this design intent.
2. Total savings of ~80 μs per ledger represent <0.003% of baseline.
3. Changing the `e2e_invoke::invoke_host_function` signature to accept
   `Option<&ModuleCache>` would modify the soroban-env-host API, which is
   technically out of scope for bridge-layer optimizations.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-18
**Failed At**: hypothesis
**Novelty**: PASS — distinct from F001 (which investigated `ProtocolSpecificModuleCache::shallow_clone()` per cluster, not `ModuleCache::clone()` per TX)

### Why It Failed

The per-TX cost of cloning `ModuleCache` is ~15-30 ns (3 Arc increments for
a shallow copy), confirmed by the explicit design comment in module_cache.rs.
Total savings of ~80 μs per ledger are negligible (<0.003%). Additionally,
the fix would require changing the soroban-env-host public API, which is out
of scope.

### Lesson Learned

Per-TX Arc-backed shallow clones are effectively free (~15-30 ns). The
soroban-env-host team explicitly designed `ModuleCache::clone()` to be a
shallow copy. The per-TX clone in `soroban_proto_all.rs:134` is by design,
not an oversight.
