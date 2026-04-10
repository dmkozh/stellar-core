# H026: Batch Multiple TXs Into a Single Bridge Call to Amortize Per-Call Overhead

**Date**: 2026-04-10
**Subsystem**: soroban-env (bridge layer)
**Severity**: Informational
**Impact**: Reduce per-TX fixed overhead by amortizing bridge call setup across multiple TXs
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

Instead of calling `invoke_host_function` once per TX across the C++↔Rust
bridge, batch all TXs in a cluster into a single bridge call. The Rust side
would iterate over the batch, executing each TX sequentially within a single
FFI crossing. This would amortize per-call fixed costs: function dispatch,
parameter marshaling, Budget construction, Host setup, and Host teardown.

## Mechanism

Each `invoke_host_function` call has fixed overhead independent of TX complexity:
- CXX function dispatch: ~50 ns
- Budget construction (`Budget::try_from_configs`): ~500 ns–1 μs
- Host construction + storage map building: ~3–10 μs
- Host teardown (`try_finish` + `get_ledger_changes`): ~2–5 μs
- Total per-call overhead: ~6–16 μs

For 3000 TXs: 3000 × ~10 μs = ~30 ms → ~4.7% of 640 ms baseline.

A batched API would construct Budget once (with max limits), build the storage
map once (union of all footprints), and execute TXs sequentially within the
host, resetting per-TX state between executions.

## Trigger

Any apply-load benchmark scenario with many TXs per cluster.

## Target Code

- `src/rust/src/bridge.rs:193-208` — Current per-TX bridge signature
- `src/rust/src/soroban_proto_any.rs:391-567` — Per-TX invocation wrapper
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:408-521` — Per-TX Host construction/teardown (out of scope)

## Evidence

- Per-TX Host construction + Budget creation + storage map building is visible overhead (~6–16 μs per TX)
- All TXs in a cluster share the same ledger state and cost parameters
- The Module Cache is already shared across TXs (via Arc)

## Anti-Evidence

- Host construction, Budget, and storage map building are all in `soroban-env-host` internals (out of scope)
- The `e2e_invoke::invoke_host_function` API creates and destroys a Host per call — no batch API exists
- Each TX mutates storage (RW entries) that subsequent TXs must see — the batch would need sequential storage updates between TXs
- Budget limits differ per TX (each TX has its own `instruction_limit`)

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated as a batching approach

### Why It Failed

The per-call fixed overhead (~6–16 μs) is dominated by Host/Budget/StorageMap
construction in `soroban-env-host`, which is out of scope. The bridge layer's
own contribution (CXX dispatch, parameter marshaling) is ~1–2 μs per call,
yielding only ~3–6 ms over 3000 TXs (<1% of baseline).

Furthermore, a batch API would require a new `invoke_host_functions_batch`
entry point in `soroban-env-host` that manages per-TX state resets while
keeping the Host alive. This is a fundamental architectural change to the
Soroban execution model, not a bridge-layer optimization.

Each TX also has unique `instruction_limit`, `auth_entries`, `hostFunction`,
and RW footprint mutations, making state management between batched TXs
complex and error-prone.

### Lesson Learned

The CXX FFI crossing itself (~50 ns function call) is negligible. Per-call
overhead is dominated by Rust-side Host/Budget/StorageMap construction, which
is inside `soroban-env-host` and out of scope for bridge-layer optimization.
Batching approaches must target the host internals to achieve meaningful
savings, which requires changes outside the optimization scope.
