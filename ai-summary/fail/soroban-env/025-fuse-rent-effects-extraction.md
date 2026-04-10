# H025: Fuse extract_rent_changes and extract_ledger_effects Into Single Pass

**Date**: 2026-04-10
**Subsystem**: soroban-env (bridge layer)
**Severity**: Informational
**Impact**: Eliminate one redundant iteration over ledger_changes vector
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After `invoke_host_function` succeeds, the Rust bridge should extract rent
changes and ledger effects in a single pass over `res.ledger_changes` instead
of two separate iterations. Currently (soroban_proto_any.rs:491-497):

```rust
let rent_changes = extract_rent_changes(&res.ledger_changes);
let rent_fee = host_compute_rent_fee(&rent_changes, ...);
let modified_ledger_entries = extract_ledger_effects(res.ledger_changes)?;
```

`extract_rent_changes` iterates `ledger_changes` to build `Vec<LedgerEntryRentChange>`,
then `extract_ledger_effects` iterates again (consuming) to build
`Vec<RustBuf>`. Fusing would iterate once, computing rent changes inline and
building the output entries simultaneously.

## Mechanism

Two iterations over the same ~5–10 element vector. The first
(`extract_rent_changes`) reads fields to build `LedgerEntryRentChange` structs.
The second (`extract_ledger_effects`) encodes modified entries as XDR bytes and
constructs TTL entries. Fusing saves one loop + one `Vec<LedgerEntryRentChange>`
allocation.

Per-TX savings: ~200–500 ns (one Vec allocation for ~5 items + one iteration
over ~5–10 elements). Over 6400 TXs: ~1.3–3.2 ms. Against ~640–750 ms
baseline: <0.5%.

## Trigger

Any apply-load benchmark scenario. The two-pass pattern runs on every
successful TX invocation.

## Target Code

- `src/rust/src/soroban_proto_any.rs:491-497` — Sequential `extract_rent_changes` then `extract_ledger_effects` calls
- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects` implementation
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:959-1053` — `extract_rent_changes` in soroban-env-host (out of scope for modification)

## Evidence

- Two clearly separate iterations over the same `ledger_changes` vector
- Both functions access overlapping fields (old/new TTL values, entry existence)
- Vec allocation for rent_changes is unnecessary if rent fee is computed inline

## Anti-Evidence

- Savings are ~200–500 ns per TX, well below the benchmark noise floor
- `extract_rent_changes` is defined in soroban-env-host (out of scope); only the bridge could implement a fused version using the `LedgerChange` fields directly
- Code clarity: two separate passes with distinct responsibilities is easier to maintain

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The savings of ~200–500 ns per TX (1.3–3.2 ms over 6400 TXs) represent <0.5%
of the baseline runtime. This is well below the benchmark's 1–2% noise floor
and cannot be reliably measured. Additionally, `extract_rent_changes` is defined
in `soroban-env-host` (out of scope), so fusing would require either duplicating
its logic in the bridge layer or modifying the soroban-env-host API.

### Lesson Learned

For vectors with ~5–10 elements, the cost of an extra iteration is O(10) × ~20
ns = ~200 ns. This is negligible even when multiplied across thousands of TXs.
Two-pass algorithms over small collections are not optimization targets for the
bridge layer.
