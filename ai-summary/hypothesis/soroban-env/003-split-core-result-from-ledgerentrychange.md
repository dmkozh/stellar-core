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
