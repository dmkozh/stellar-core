# H002: Hash InvokeHostFunctionSuccessPreImage From Raw XDR Bytes

**Date**: 2025-07-14
**Subsystem**: soroban
**Severity**: Low
**Impact**: Reduced per-TX CPU in parallel phase
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When computing the success preimage hash for each Soroban transaction,
`xdrSha256(success)` should hash the transaction's events and return value
as efficiently as possible. Since events and the return value arrive from
Rust as pre-encoded XDR bytes, the hash should be computed directly from
those raw bytes rather than from decoded-then-re-traversed C++ objects.

## Mechanism

The per-TX pipeline has a decode→re-hash round-trip:

1. `collectEvents` (InvokeHostFunctionOpFrame.cpp:763-811) decodes each
   event from raw XDR bytes via `xdr::xdr_from_opaque(buf.data, evt)`.
2. `finalizeSuccess` (line 876) decodes the return value from raw XDR.
3. `finalizeSuccess` (line 878) calls `xdrSha256(success)` which uses an
   `XDRSHA256` archiver (SHA.h:37-61) to recursively walk the decoded
   `InvokeHostFunctionSuccessPreImage` structure field-by-field, hashing
   each primitive via individual `SHA256::add` calls.

The archiver makes ~10-20 `SHA256::add` calls per event (one per XDR field
in the recursive traversal of `ContractEvent` → `ContractEventBody` →
`SCVal` topics/data). In contrast, hashing from raw bytes requires only 1
call per event (the entire pre-encoded XDR blob).

The hash can be computed incrementally from raw bytes by constructing the
XDR vector framing manually:
```
SHA256::add(uint32_be(event_count))      // xdr::xvector length prefix
for each event:
    SHA256::add(raw_event_bytes)          // already XDR-encoded from Rust
SHA256::add(raw_return_value_bytes)       // already XDR-encoded from Rust
```

This produces the identical hash because XDR encoding is deterministic and
the raw bytes from Rust ARE the canonical XDR encoding.

The decoded events are still needed for `setEvents` (line 882), so this
optimization doesn't eliminate decoding — it replaces the archiver's
recursive traversal during hashing with direct block hashing of raw bytes.

## Trigger

Run apply-load benchmark with 3200 SAC transactions. Each TX produces ~3
events. Total: ~9,600 events decoded then re-traversed for hashing. Profile
the `xdrSha256` call in `finalizeSuccess` vs. direct `SHA256::add` on raw
bytes.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:finalizeSuccess:873-886` — calls xdrSha256(success)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:collectEvents:763-811` — decodes events from raw XDR
- `src/crypto/SHA.h:XDRSHA256:37-45` — streaming XDR hasher with per-field add calls
- `src/crypto/SHA.h:xdrSha256:55-61` — template function that archives+hashes

## Evidence

- Events arrive as `Vec<RustBuf>` from Rust (bridge.rs:52), each containing pre-encoded XDR bytes
- `xdr_from_opaque` decodes them to C++ objects (line 792)
- `xdrSha256` then re-walks those objects field-by-field through the XDR archiver
- The XDR archiver makes many small `SHA256::add` calls vs. one large call per event from raw bytes
- SHA256 is more efficient with fewer, larger updates due to reduced function call overhead
- Return value follows the same pattern (decoded at line 876, re-hashed at line 878)

## Anti-Evidence

- `XDRSHA256` uses `SHA256::add(ByteSlice)` which internally buffers to SHA256 block boundaries, so per-field overhead is mostly function call dispatch (~5ns each)
- With ~3 events per TX × ~15 extra calls per event × 5ns = ~225ns per TX savings
- At 3200 TXs: ~0.7ms total savings — measurable but small
- Implementation requires manually constructing XDR framing bytes (vector length prefix), adding a correctness risk
- If event structure changes, the manual framing code needs updating
