# H003: Split the Core Enforcing Result from `LedgerEntryChange` Materialization

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / output marshaling and rent accounting
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The enforcing bridge path used by stellar-core should compute only the outputs
that core actually consumes on success: encoded return value, encoded contract
events, a distilled rent-change list, and the modified/deleted ledger effects.
It should not build a full `Vec<LedgerEntryChange>` for every footprint item
when core immediately collapses that vector into `extract_rent_changes()` and
`extract_ledger_effects()`.

## Mechanism

`e2e_invoke::invoke_host_function` always calls `get_ledger_changes`, which
allocates one `LedgerEntryChange` per footprint item and eagerly encodes
`encoded_key`; for any pre-existing entry it also re-encodes the old entry to
recover its XDR size for rent accounting. Immediately afterward,
`soroban_proto_any::invoke_host_function_or_maybe_panic` throws away most of
that structure: it extracts `Vec<LedgerEntryRentChange>` for `rent_fee`,
extracts `Vec<RustBuf>` for modified entries, and discards `encoded_key`,
`read_only`, and most unchanged/no-op entries.

That means the core path pays simulation/recording-oriented output costs that it
never uses. A core-specific result helper — separate from the existing
recording/simulation-friendly `LedgerEntryChange` flow — could emit only the
filtered rent changes and modified outputs, avoiding intermediate allocation and
several per-entry `metered_write_xdr` passes.

## Trigger

Run any successful apply-load scenario. Every successful Soroban transaction
hits the `get_ledger_changes` → `extract_rent_changes` / `extract_ledger_effects`
collapse, including transactions whose footprint includes unchanged read-only
entries.

## Target Code

- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:41-59` — current enforcing result carries `ledger_changes`
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:180-293` — `get_ledger_changes` allocates and encodes per-footprint change records
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:324-366` — `extract_rent_changes` immediately distills the richer structure down to rent-only fields
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:493-510` — success path always materializes `ledger_changes` before returning
- `src/rust/src/soroban_proto_any.rs:488-497` — bridge immediately converts `ledger_changes` into `rent_changes` and `modified_ledger_entries`

## Evidence

The existing enforcing path clearly builds more structure than core needs. In
`get_ledger_changes`, every storage item pays `metered_write_xdr` for the key,
and every pre-existing item pays another `metered_write_xdr` for the old entry
before the bridge even knows whether that information will survive the
`extract_rent_changes` / `extract_ledger_effects` narrowing step. Classic
entries have no TTL-based rent change at all, yet they still flow through the
same rich change object before being dropped.

## Anti-Evidence

Recording mode and simulation still need the current `LedgerEntryChange` shape,
so this likely has to be an additional core-specific helper rather than a full
replacement. Some output-side work remains unavoidable: core still needs
encoded modified entries, encoded events, and rent changes for correctness.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full path from the bridge layer (`soroban_proto_any.rs:488-497`) through the host crate's `invoke_host_function` (`e2e_invoke.rs:493-510`) into `get_ledger_changes` (`e2e_invoke.rs:183-293`). Confirmed the hypothesis correctly identifies real waste: `encoded_key` is serialized for every footprint item (line 208) but never consumed by `extract_rent_changes` or `extract_ledger_effects`; old entries are re-serialized (lines 227-228) solely to compute `buf.len()` for rent size. However, all this work occurs inside `soroban-env-host` crate internals, not in the bridge layer.

### Code Paths Examined

- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:183-293` — `get_ledger_changes` allocates `Vec<LedgerEntryChange>`, serializes keys (line 208) and old entries (lines 227-228) for every footprint item. This is inside the host crate.
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:493-510` — `invoke_host_function` calls `get_ledger_changes` internally and returns `InvokeHostFunctionResult` with the full `Vec<LedgerEntryChange>`. The bridge cannot intervene.
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:324-366` — `extract_rent_changes` is a public function in the host crate that filters the already-materialized changes.
- `src/rust/src/soroban_proto_any.rs:261-302` — `extract_ledger_effects` in the bridge layer consumes the full `Vec<LedgerEntryChange>`, keeping only modified entries and TTL changes.
- `src/rust/src/soroban_proto_any.rs:488-497` — bridge calls `extract_rent_changes` then `extract_ledger_effects` on the result it receives from the host.

### Why It Failed

**Out of scope**: The optimization requires modifying `get_ledger_changes` and/or adding a new core-specific result function inside `soroban-env-host/src/e2e_invoke.rs`. The scope note explicitly states: "If an optimization requires changes inside `soroban-env-host` crate internals, it is out of scope for this objective." The bridge layer (`soroban_proto_any.rs`) only receives the final `InvokeHostFunctionResult` after all the wasteful allocations and serializations have already occurred inside the host crate. There is no way to avoid the waste from the bridge side — by the time the bridge touches the data, the work is done.

### Lesson Learned

When a hypothesis identifies waste in the host crate's result-construction path (e.g., `get_ledger_changes`), verify whether the fix can be applied purely in the bridge layer. If the host crate's public API (`invoke_host_function` → `InvokeHostFunctionResult`) already embeds the wasteful work, the bridge cannot avoid it without modifying the host crate internals. This aligns with fail meta-pattern #7: "When dominant cost is inside an out-of-scope crate's API boundary, bridge-layer optimization is limited to data preparation before and result processing after the call."
