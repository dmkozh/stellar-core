# H007: Cache BudgetImpl Template to Avoid Per-TX Cost Model Reconstruction

**Date**: 2025-07-22
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: Eliminate per-TX Budget::try_from_configs overhead by caching pre-built cost models
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The `Budget::try_from_configs()` call that constructs a per-TX budget should
reuse pre-built cost models from a cached template rather than reconstructing
~28 `MeteredCostComponent` entries from `ContractCostParams` XDR on every
transaction. Since cost params are identical within a ledger close, the cost
models (linear `const_term + lin_term * input` coefficients) are the same for
every TX — only the `cpu_limit` and `mem_limit` differ per TX.

## Mechanism

`invoke_host_function_or_maybe_panic()` (soroban_proto_any.rs:425-430) calls
`Budget::try_from_configs(cpu_limit, mem_limit, cpu_cost_params.clone(),
mem_cost_params.clone())` per TX. This:

1. Clones `ContractCostParams` × 2 (~672 bytes each, ~100-200ns total)
2. Inside `try_from_configs()`: iterates ~28 entries per dimension (×2 for
   CPU/mem), extracts i128 const_term and i128 lin_term, converts to i64,
   builds `MeteredCostComponent` for each (~56 entries total)
3. Calls `load_calibrated_fuel_costs()` to build wasmi fuel cost table

Steps 2-3 happen inside `soroban-env-host` (Budget is
`Rc<RefCell<BudgetImpl>>`) which is out of scope for direct modification.
However, the **bridge layer** could cache a Budget object and expose a
hypothetical `Budget::clone_and_reset(cpu_limit, mem_limit)` method that
clones the internal state (keeping cost models) and resets consumption
tracking + limits. This would replace the full reconstruction with a cheap
clone + limit update.

The per-TX saving would be ~1-3μs (cost model reconstruction from XDR
params). For 6400 SAC TXs: ~6.4-19.2ms. Against ~850ms T=1 baseline:
~0.8-2.3%.

## Trigger

Run apply-load with any scenario. Every TX calls `Budget::try_from_configs`.

## Target Code

- `src/rust/src/soroban_proto_any.rs:425-430` — `Budget::try_from_configs` call site
- `soroban-env-host/src/budget.rs` — `Budget::try_from_configs` implementation (out of scope for modification)
- `soroban-env-host/src/budget/dimension.rs` — `BudgetDimension` cost model building

## Evidence

- `try_from_configs` is called per TX with identical cost params (only limits differ)
- Cost model reconstruction processes ~56 entries with i128→i64 conversion per call
- `load_calibrated_fuel_costs` builds a fixed-size fuel cost table from cost models per call
- The `ContractCostParams.clone()` overhead alone is ~100-200ns × 2 per TX

## Anti-Evidence

- `Budget`, `BudgetImpl`, and `BudgetDimension` are all defined in soroban-env-host with `pub(crate)` visibility — the bridge layer cannot access internal fields
- No `Budget::clone_and_reset()` API exists — implementing this requires adding a new public method to soroban-env-host, which is out of scope
- The `Budget` is `Rc<RefCell<BudgetImpl>>` — cloning creates a new Rc, but the BudgetImpl contains `BudgetTracker` with per-cost-type arrays that need zeroing
- Even if we could clone, `BudgetImpl` doesn't derive `Clone` (it uses `RefCell` internally)
- The ~1-3μs per TX estimate may be optimistic — modern CPUs handle the loop of 56 simple conversions quickly

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (success #001 cached cost params deserialization; this targets Budget construction from already-deserialized params)

### Why It Failed

The optimization requires adding a new public API (`Budget::clone_and_reset`)
to `soroban-env-host`, which is explicitly out of scope. The bridge layer
cannot access `BudgetImpl` internals due to `pub(crate)` visibility. Even if
feasible, the per-TX saving (~1-3μs) totals ~0.8-2.3% of benchmark time —
below the 5% Low severity threshold.

### Lesson Learned

When the dominant cost is inside an out-of-scope crate's API boundary, the
bridge layer can only optimize data preparation BEFORE the API call (which
success #001 already did for deserialization) or result processing AFTER it.
Optimizing the API call itself requires upstream changes to that crate. The
`Budget::try_from_configs` call is a good example: it's the most expensive
remaining per-TX bridge operation, but fixing it requires soroban-env-host
changes.
