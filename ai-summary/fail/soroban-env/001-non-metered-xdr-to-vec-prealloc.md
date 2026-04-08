# H001-FAIL: non_metered_xdr_to_vec Lacks Pre-allocation

**Date**: 2026-04-08
**Subsystem**: soroban-env (C++↔Rust bridge)
**Severity**: Low
**Impact**: Reduced allocation churn in XDR output serialization
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

`non_metered_xdr_to_vec<T>` should pre-allocate the output `Vec<u8>` with a
reasonable capacity estimate before writing XDR, avoiding multiple
reallocations as the vector grows from capacity 0.

## Mechanism

`non_metered_xdr_to_vec` (soroban_proto_any.rs:149-160) starts with
`Vec::new()` (capacity 0) and writes XDR via `Cursor::new(&mut vec)`. As data
is written, the Vec reallocates multiple times (0→8→16→32→64→128...). For a
TtlEntry (~50 bytes), this means ~4-5 reallocations.

## Trigger

Process transactions that modify TTL entries (the main caller of this function
via `extract_ledger_effects`).

## Target Code

- `src/rust/src/soroban_proto_any.rs:non_metered_xdr_to_vec:149-160` — Zero-capacity Vec initialization
- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-302` — Caller for TTL entry encoding
- `src/rust/src/soroban_proto_any.rs:encode_diagnostic_events:248-259` — Caller for diagnostic events

## Evidence

`Vec::new()` at line 150 creates a vector with zero capacity. The `write_xdr`
call writes bytes incrementally, triggering the standard doubling growth pattern.

## Anti-Evidence

The function is only called in `extract_ledger_effects` (for TTL entries, ~50
bytes each, small count) and `encode_diagnostic_events` (empty when diagnostics
disabled). The main output paths (modified entries, contract events, invoke
result) use pre-encoded bytes from the soroban host and never call this function.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The function is not on the hot path for the apply-load benchmark. The main
output serialization paths (modified ledger entries via `encoded_new_value`,
contract events via `encoded_contract_events`, invoke result via
`encoded_invoke_result`) all come pre-encoded from the soroban host and bypass
`non_metered_xdr_to_vec` entirely. The only callers are:

1. `extract_ledger_effects` — constructs TtlEntry objects (~50 bytes each) for
   TTL changes. Typical count is 1-5 per transaction. Total allocation churn
   saved: <500 bytes.
2. `encode_diagnostic_events` — empty when diagnostics disabled (normal
   production and benchmark mode).

The total performance impact of pre-allocating these tiny, infrequent
serializations is negligible (<0.1% of per-TX time).

### Lesson Learned

When evaluating serialization optimizations in the bridge, distinguish between
the pre-encoded paths (where the soroban host produces bytes directly) and the
bridge's own serialization (which is limited to TTL entries and diagnostics).
The pre-encoded paths handle the bulk of the data.
