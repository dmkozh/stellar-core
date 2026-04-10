# H001: Cache Decoded Read-Only Entry/TTL Pairs in the Rust Bridge

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / Rust-side input decoding
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Repeated immutable read-only footprint entries should be decoded from XDR once
per apply-thread / protocol cache and then reused across invocations. The
bridge should not re-run `metered_from_xdr_with_budget::<LedgerEntry>` and
`::<TtlEntry>` for the same contract code / contract instance bytes on every
transaction in the same ledger close.

## Mechanism

`invoke_host_function` always rebuilds the storage map from encoded entry
buffers, and `build_storage_map_from_xdr_ledger_entries` blindly decodes every
ledger entry / TTL pair on every call. In the benchmark workloads, many of
those read-only entries are stable across hundreds or thousands of
transactions: custom-token transfers reuse the same token code + instance, and
soroswap swaps reuse the same router / pool code and contract-instance keys.

`ProtocolSpecificModuleCache` already exists as per-protocol mutable state that
is cloned once per apply thread and reused across many transactions. Extending
it with a read-only entry cache keyed by `(footprint key, serialized bytes)` (or
equivalent cache identity) would let the Rust bridge hand back pre-decoded
`Rc<LedgerEntry>` / `Rc<TtlEntry>` pairs for immutable inputs, avoiding repeated
XDR parse + allocation work on the hot path.

## Trigger

Run the standard apply-load `custom_token` or `soroswap` scenarios. Every
transaction in those workloads repeatedly imports the same read-only contract
code / contract-instance entries into Rust before host execution starts.

## Target Code

- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:408-447` — enforcing path always rebuilds storage from encoded buffers
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:959-1045` — per-entry `metered_from_xdr_with_budget` of `LedgerEntry` / `TtlEntry`
- `src/rust/src/soroban_proto_any.rs:711-831` — `ProtocolSpecificModuleCache` already carries reusable per-protocol cache state
- `src/rust/src/soroban_module_cache.rs:22-60` — module-cache clones are reused across many invocations
- `src/simulation/TxGenerator.cpp:840-865` — custom-token transfers reuse `instance.readOnlyKeys`
- `src/simulation/ApplyLoad.cpp:3075-3205` — soroswap benchmark generates many swaps against shared contracts
- `src/simulation/TxGenerator.cpp:3140-3149` — soroswap swaps reuse router / SAC / pair read-only keys
- `src/rust/src/soroban_test_wasm.rs:116-138` — apply-load token / soroswap Wasm blobs are embedded immutable assets

## Evidence

The current Rust boundary performs the expensive part of the read-only path
even when C++ side caching is perfect: it still parses the same encoded
`LedgerEntry` and `TtlEntry` payloads into fresh Rust objects on every
invocation. The benchmark generators make that repetition explicit by reusing
the same read-only keys across every custom-token and soroswap transaction, and
the existing `ProtocolSpecificModuleCache` is an obvious place to attach a
decoded-entry cache without introducing a brand-new lifetime mechanism.

## Anti-Evidence

The cache needs careful invalidation across protocol changes and any ledger
state transition that changes the serialized bytes for a supposedly cached key.
Savings are concentrated in T=1 and in the read-only-heavy scenarios; read-write
entries remain transaction-unique and still have to be decoded every time.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — substantially overlaps with fail 008/012 (cache/share pre-serialized read-only footprint entries)
**Failed At**: reviewer

### Trace Summary

Traced the full invocation path from C++ through the bridge layer (`soroban_proto_any.rs:invoke_host_function` → `invoke_host_function_or_maybe_panic`) into the protocol adapter (`soroban_proto_all.rs:invoke_host_function_with_trace_hook_and_module_cache`) and finally into `soroban-env-host`'s `e2e_invoke::invoke_host_function`. The XDR decoding via `metered_from_xdr_with_budget` occurs inside `build_storage_map_from_xdr_ledger_entries` at `e2e_invoke.rs:959-1000`, which is entirely within the `soroban-env-host` crate. The bridge layer only passes raw `CxxBuf` byte iterators through to this function — it has no mechanism to intercept or substitute pre-decoded entries.

### Code Paths Examined

- `src/rust/src/soroban_proto_any.rs:310-354` — bridge `invoke_host_function` receives `&Vec<CxxBuf>` entries, passes as iterators
- `src/rust/src/soroban_proto_any.rs:391-462` — `invoke_host_function_or_maybe_panic` passes entries through to adapter
- `src/rust/src/soroban_proto_all.rs:784-817` — p24 adapter (same pattern for all protocols) forwards encoded iterators to `e2e_invoke::invoke_host_function_with_trace_hook`
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:408-447` — `invoke_host_function` calls `build_storage_map_from_xdr_ledger_entries` with encoded iterators
- `src/rust/soroban/p26/soroban-env-host/src/e2e_invoke.rs:959-1000` — `build_storage_map_from_xdr_ledger_entries` decodes every entry using `metered_from_xdr_with_budget`, with budget charging per entry

### Why It Failed

Two independent reasons:

1. **Out of scope**: The XDR decoding of ledger entries happens inside `soroban-env-host`'s `e2e_invoke.rs:build_storage_map_from_xdr_ledger_entries`, not in the bridge layer. The `e2e_invoke` API only accepts `T: AsRef<[u8]>` encoded byte iterators — there is no API to pass pre-decoded `LedgerEntry`/`TtlEntry` objects. Implementing this hypothesis requires either (a) modifying `e2e_invoke` inside `soroban-env-host` to accept pre-decoded entries, or (b) replicating most of `e2e_invoke::invoke_host_function`'s logic in the bridge layer. Both are explicitly out of scope per the soroban-env scope constraint: "If an optimization requires changes inside `soroban-env-host` crate internals, it is out of scope."

2. **Budget metering correctness**: The decoding uses `metered_from_xdr_with_budget`, which charges the per-TX `Budget` for CPU and memory. Bypassing this with cached entries would undercount budget consumption, breaking metering correctness. Any cache that skips metered decoding would change the observable budget behavior of transactions.

3. **Substantial overlap with fail 008/012**: The earlier investigation (cache/share pre-serialized read-only footprint entries) addressed the same inefficiency from the C++ side and was rejected because CxxBuf's unique ownership forces per-TX memcpy regardless of caching, and aggregate savings were <0.3% of baseline.

### Lesson Learned

When the wasteful operation (XDR decoding) is inside an out-of-scope crate (`soroban-env-host`) and the only bridge-layer API accepts encoded bytes, the bridge layer cannot cache decoded results. Additionally, metered operations in Soroban are part of the correctness contract — caching to skip budget charges is not just an optimization but a protocol behavior change.
