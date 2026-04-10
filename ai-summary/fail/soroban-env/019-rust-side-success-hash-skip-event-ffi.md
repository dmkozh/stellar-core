# H001: Compute Success Hash on Rust Side and Skip Event FFI Transfer When Meta Disabled

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: CPU / FFI data transfer / output marshaling
**Hypothesis by**: claude-opus-4.6, high

## Relationship to Reviewed H002

Reviewed H002 proposes hashing the success preimage from raw XDR bytes on the
C++ side. This hypothesis proposes a more aggressive variant: compute the hash
entirely on the Rust side (where the pre-encoded bytes originate) and return the
hash as part of `InvokeHostFunctionOutput`. When metadata output is disabled
(as in the benchmark config), this additionally eliminates the need to pass
event buffers across the FFI boundary at all — saving both the CxxBuf
allocations for events and the byte copies through the CXX bridge.

## Expected Behavior

When a Soroban transaction succeeds, the system must compute
`SHA256(XDR(InvokeHostFunctionSuccessPreImage))` where the preimage contains
`{returnValue: SCVal, events: xdr::xvector<ContractEvent>}`. The Rust host
already holds the canonical XDR bytes of both the return value
(`encoded_invoke_result`) and each contract event (`encoded_contract_events`).

Expected: the hash should be computed incrementally from these pre-encoded byte
slices on the Rust side using SHA-256's streaming interface, without any
decode/re-encode cycle. When metadata output is disabled, the raw event buffers
should not cross the FFI boundary at all.

## Mechanism

Currently, the Rust bridge wraps pre-encoded XDR bytes as `RustBuf` vectors and
returns them to C++ in `InvokeHostFunctionOutput`. On the C++ side,
`collectEvents()` deserializes each event buffer via `xdr_from_opaque()` into
`ContractEvent` objects, and `finalizeSuccess()` deserializes the return value,
then `xdrSha256(success)` walks the C++ object tree to re-encode and hash the
`InvokeHostFunctionSuccessPreImage`. When metadata is disabled (benchmark
config), `OpEventManager::setEvents()` early-returns at line 506-508 and
`setSorobanReturnValue()` is also a no-op — so the decoded objects are
constructed purely for the hash, then discarded.

By computing the hash on the Rust side:
1. The decode+re-encode cycle is eliminated entirely (for all configurations)
2. When meta is disabled, event buffers don't need to be wrapped in `RustBuf`,
   copied through CXX, or allocated as `CxxBuf` on the C++ side
3. The Rust-side hash uses `SHA256::new()` + incremental `update()` calls on
   the existing byte slices, which is strictly cheaper than the C++ path

The XDR encoding of `InvokeHostFunctionSuccessPreImage` is canonically:
```
XDR(returnValue: SCVal) || big_endian_u32(events.length) || XDR(events[0]) || ... || XDR(events[n-1])
```
Since Rust's `stellar-xdr` and C++'s xdrpp both produce canonical RFC 4506 XDR,
the raw bytes from Rust are byte-identical to what `xdrSha256(success)` would
re-produce after decoding and re-encoding.

## Trigger

Run the standard apply-load benchmark matrix (`scripts/run_apply_load_matrix.py`)
with SAC, custom_token, and soroswap scenarios. All emit contract events that
exercise this path. The soroswap scenario (4+ events per TX) should show the
largest absolute savings per TX.

## Target Code

- `src/rust/src/soroban_proto_any.rs:488-516` — success path assembles `RustBuf` vectors; add incremental SHA-256 hash here
- `src/rust/src/soroban_proto_any.rs:261-301` — `extract_ledger_effects` builds event output; conditionally skip event buffer construction
- `src/rust/src/bridge.rs:34-55` — `InvokeHostFunctionOutput` struct; add `success_preimage_hash: [u8; 32]` field, conditionally omit `contract_events`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:706-753` — `collectEvents()` decodes each event via `xdr_from_opaque`; skip when meta disabled and hash pre-computed
- `src/transactions/InvokeHostFunctionOpFrame.cpp:816-829` — `finalizeSuccess()` decodes result and computes `xdrSha256(success)`; use pre-computed hash instead
- `src/transactions/EventManager.cpp:504-508` — `setEvents()` already no-ops when meta disabled, confirming events are unused

## Evidence

1. The Rust host's `encode_contract_events()` produces canonical XDR bytes that
   are byte-identical to what the C++ `xdrSha256` re-encodes after round-tripping
   through decode. This was confirmed in reviewed H002's trace.
2. `OpEventManager::setEvents()` returns immediately when `!mEnabled` (line 506-508),
   and `setSorobanReturnValue()` is also a no-op when meta is disabled. So the
   decoded C++ objects serve no purpose beyond the hash in benchmark mode.
3. Size validation in `collectEvents()` uses only `buf.data.size()` (raw byte
   lengths), not decoded event content, so validation can proceed without decoding.
4. SHA-256 supports incremental hashing. The Rust side can feed raw byte slices
   directly without allocating a contiguous buffer.

Per-TX savings estimate (SAC, 1 event):
- Skip event decode: ~300-500ns
- Skip result decode: ~100-200ns
- Skip xdrSha256 C++ tree walk: ~400-600ns
- Skip event CxxBuf alloc+copy (meta off): ~100-150ns
- Rust incremental hash cost: ~200-300ns
- Net savings: ~700-1150ns per TX
- 6400 TXs: ~4.5-7.4ms (~0.5-0.9% of 850ms baseline)

Per-TX savings estimate (soroswap, 4 events):
- Skip event decode: ~1200-2000ns
- Skip result decode: ~100-200ns
- Skip xdrSha256 tree walk: ~800-1200ns
- Skip event CxxBuf alloc+copy (meta off): ~400-600ns
- Rust hash cost: ~400-600ns
- Net savings: ~2100-3400ns per TX
- 1600 TXs: ~3.4-5.4ms (~0.5-0.8% of 713ms baseline)

Combined with reviewed H001 (cost param caching) and H003 (LedgerEntryChange
preservation), these bridge optimizations accumulate.

## Anti-Evidence

1. The per-TX savings (~700-3400ns) are individually small relative to the
   ~130-200μs total per-TX apply time. The percentage improvement (0.5-0.9%)
   is at the boundary of measurability.
2. Requires modifying both the Rust bridge struct (`InvokeHostFunctionOutput`)
   and C++ processing logic, plus adding a `meta_enabled` flag to the FFI
   interface so Rust knows whether to skip event buffer construction.
3. The correctness sensitivity is high: the Rust-side hash must exactly match
   what C++ would compute. Any XDR encoding divergence would produce consensus
   failures. Extensive testing is required.
4. When metadata IS enabled (production), events still need to cross FFI for
   meta population, so savings are reduced to just the hash computation
   (~400-800ns per TX).

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of reviewed soroban-env/H002 and reviewed soroban/H004
**Failed At**: reviewer

### Trace Summary

Traced the complete success path from Rust host output through FFI to C++ processing. Confirmed the claimed inefficiency is real (decode + re-encode cycle for hashing). However, this optimization is already fully captured by reviewed H002 ("Hash Success Preimage From Returned XDR and Skip C++ Decode When Meta Is Off") which proposes raw-byte hashing on the C++ side. Reviewed soroban/H004 covers the identical optimization from a different subsystem angle. The ONLY incremental claim of H001 beyond H002 is moving the hash to Rust and skipping event FFI transfer — but the incremental savings are negligibly small.

### Code Paths Examined

- `src/rust/src/soroban_proto_any.rs:488-516` — Success path wraps `encoded_invoke_result` and `encoded_contract_events` as `RustBuf` vectors. The `.into()` and `.map(RustBuf::from).collect()` operations involve lightweight `Vec<u8>` wrapping, not deep copies.
- `src/rust/src/bridge.rs:34-55` — `InvokeHostFunctionOutput.contract_events` is `Vec<RustBuf>`. CXX transfers these as `rust::Vec<RustBuf>` which involves pointer-level moves, not byte-level copies.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:706-753` — `collectEvents()` iterates event buffers. The `buf.data.size()` calls for size validation are cheap. The `xdr_from_opaque` decode at line 735 is the expensive part — already addressed by H002.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:816-829` — `finalizeSuccess()` decode + hash — already addressed by H002.

### Why It Failed

This hypothesis is substantially equivalent to already-reviewed H002 (soroban-env) and H004 (soroban). The core optimization — hash from raw XDR bytes and skip C++ decode when meta is off — is identical across all three.

The only novel claim is that computing the hash on the Rust side and skipping event buffer FFI transfer saves additional time. Analysis of the incremental savings:

1. **Hash computation location (Rust vs C++)**: The SHA-256 computation cost is identical regardless of which side performs it (~200-300ns). No savings from relocation.

2. **Skipping event FFI transfer**: CXX transfers `rust::Vec<RustBuf>` by moving pointers, not copying bytes. The `RustBuf::from(Vec<u8>)` wrapping on the Rust side is a zero-copy wrapper. The actual FFI transfer cost per event is ~10-30ns (pointer move + metadata), not the ~100-150ns estimated in the hypothesis. For SAC (1 event): ~10-30ns. For soroswap (4 events): ~40-120ns. At 6400 TXs: 0.06-0.19ms — completely unmeasurable against ~750ms baseline.

3. **Added complexity**: Requires a new `meta_enabled` FFI parameter, a `success_preimage_hash` field in `InvokeHostFunctionOutput`, conditional event buffer construction in Rust, and new SHA-256 dependency in the bridge code. This is significantly more invasive than H002's C++-only change.

The incremental improvement (~0.01-0.03%) does not justify the added FFI interface complexity, and the optimization it shares with H002 is already captured there.

### Lesson Learned

When a hypothesis positions itself as a "more aggressive variant" of an existing optimization, the incremental savings must be evaluated against the base optimization, not against the unoptimized baseline. CXX FFI transfer of `rust::Vec<RustBuf>` is pointer-level, not byte-copy-level, making FFI transfer avoidance less impactful than it appears.
