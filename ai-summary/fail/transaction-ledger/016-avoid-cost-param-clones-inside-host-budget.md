# H016: Avoid Per-Call `ContractCostParams` Clones Inside the Rust Cost-Param Cache

**Date**: 2026-04-10
**Subsystem**: transaction-ledger (soroban-env bridge / host boundary)
**Severity**: Medium
**Impact**: Rust-side cache-hit copying before budget creation
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

After the Rust bridge cache hits on unchanged cost-parameter bytes, it should
reuse those cached parameters without cloning the full `ContractCostParams`
payload on every invoke-host call.

## Mechanism

`ProtocolSpecificModuleCache::get_or_deserialize_cost_params` returns
`cached_params.clone()` on every cache hit, and `Budget::try_from_configs`
currently takes owned `ContractCostParams` values. This suggests a second round
of per-call copying remains even after deserialization caching, which looks like
an attractive follow-up optimization for lightweight apply-load workloads.

## Trigger

Run any Soroban apply-load scenario after the Rust-side cost-param cache is hot
and sample time and allocations in `get_or_deserialize_cost_params` and
`Budget::try_from_configs`.

## Target Code

- `src/rust/src/soroban_proto_any.rs:797-830` — cache hit returns `cached_params.clone()`
- `src/rust/soroban/p26/soroban-env-host/src/budget.rs:1263-1274` — `Budget::try_from_configs` takes owned `ContractCostParams`
- `src/rust/soroban/p26/soroban-env-host/src/budget.rs:210-218` — `BudgetImpl::try_from_configs` consumes those owned params

## Evidence

- The Rust cache clearly clones the cached params on hit.
- `ContractCostParams` can be large enough that repeated copies are not obviously free.

## Anti-Evidence

- The only clean fix appears to require changing `soroban-env-host` budget APIs
  to accept borrowed/shared params or restructuring budget ownership.
- That crosses into host-internal implementation rather than the bridge layer
  this objective is allowed to optimize.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The promising part of the mechanism sits behind the bridge boundary inside
`soroban-env-host` budget construction. Eliminating the clone cleanly would
require changing host-internal APIs and ownership patterns, which is out of
scope for this objective.

### Lesson Learned

When a promising bridge-adjacent optimization requires modifying
`soroban-env-host` budget internals rather than the C++↔Rust marshaling layer,
record it as out of scope and look for a shallower fix on the C++ side instead.
