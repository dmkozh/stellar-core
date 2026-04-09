# H001: Per-TX Cost Parameter Deserialization Still Dominates Bridge Time

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

For protocol 23+ Soroban transactions, repeated bridge invocations in the same
process should not repeatedly deserialize identical `cpu_cost_params` and
`mem_cost_params` XDR on every transaction. After the first use of a given
serialized value, later invocations should hit a cache and reuse the parsed
`ContractCostParams`.

## Mechanism

The suspected issue was that `invoke_host_function_or_maybe_panic` would call
`non_metered_xdr_from_cxx_buf` on both cost-parameter buffers for every
invocation, making the bridge spend measurable CPU before host execution even
started. That would have been meaningful for apply-load because every Soroban tx
constructs a `Budget` from these parameters.

## Trigger

Profile a p26 apply-load run and look for repeated `ContractCostParams`
deserialization before `Budget::try_from_configs`.

## Target Code

- `src/rust/src/soroban_proto_any.rs:invoke_host_function_or_maybe_panic:412-430` — cost-param lookup before budget construction
- `src/rust/src/soroban_proto_any.rs:ProtocolSpecificModuleCache::get_or_deserialize_cost_params:797-830` — parsed-params cache
- `src/rust/src/soroban_module_cache.rs:SorobanModuleCache::shallow_clone:54-61` — cache wrapper cloning behavior

## Evidence

The bridge does reconstruct a budget for every host invocation, and older code
patterns in this area commonly re-decoded XDR buffers on each call. The raw
buffers are still passed over the bridge in every invocation.

## Anti-Evidence

Current code already checks `get_protocol_cache(module_cache)` and uses
`get_cpu_cost_params` / `get_mem_cost_params`, which keep cached serialized bytes
and parsed `ContractCostParams` behind `RwLock`s. `git log` for this file also
shows a recent explicit optimization commit, `e47dc53c0 perf(soroban-env): cache
cost params across bridge calls`, matching the implemented defense.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The suspected optimization has already landed: p23+ bridge calls now reuse
deserialized cost parameters instead of decoding them on every transaction.

### Lesson Learned

Check recent perf commits in `soroban_proto_any.rs` before proposing bridge
marshaling wins in this area; the obvious cost-param cache has already been
addressed.
