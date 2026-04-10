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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete path from Rust host invocation output through `collectEvents` (decodes raw XDR bytes to C++ `ContractEvent` objects), through `finalizeSuccess` (decodes return value, hashes via `xdrSha256`), to `setEvents` (moves events to meta). Confirmed the decode→re-hash round-trip exists. However, the `XDRHasher` (XDRHasher.h) has a 256-byte internal buffer that batches small `queueOrHash` calls into fewer actual `SHA256::add` calls, significantly reducing the overhead the hypothesis claims.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:collectEvents:707-754` — Decodes each event from `out.contract_events` (RustBuf raw XDR) via `xdr::xdr_from_opaque` into `success.events`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:finalizeSuccess:816-829` — Decodes return value from `out.result_value.data`, calls `xdrSha256(success)` at line 821, then `setEvents` at line 825
- `src/crypto/SHA.h:xdrSha256:53-61` — Creates `XDRSHA256` archiver, calls `xdr::archive(xs, t)` which recursively traverses the XDR structure
- `src/crypto/XDRHasher.h:queueOrHash:27-49` — 256-byte buffer that batches small writes; only calls underlying `hashBytes` when buffer overflows or on data larger than 256 bytes
- `src/crypto/SHA.h:XDRSHA256::hashBytes:41-44` — Delegates to `SHA256::add(ByteSlice)`
- `src/crypto/SHA.cpp:SHA256::add:56-68` — Calls `crypto_hash_sha256_update` (libsodium), which has its own 64-byte internal buffer; also has `ZoneScoped` Tracy overhead
- `src/protocol-curr/xdr/Stellar-ledger.h:3953-3957` — XDR traits for `InvokeHostFunctionSuccessPreImage`: serializes `returnValue` first, then `events`

### Findings

The inefficiency is real but dramatically overstated by the hypothesis:

1. **XDRHasher buffer mitigates most overhead**: The 256-byte buffer in `XDRHasher` batches the many small `operator()` calls into just 1-3 actual `SHA256::add` calls per event (~200-500 bytes of XDR per event). The per-field `queueOrHash` calls are mostly just `memcpy` into this buffer (~2-5ns each).

2. **Impact estimate**: With ~30 `queueOrHash` calls per event (ContractEvent has nested SCVal topics/data) and ~2 extra buffer flushes vs. 1 direct `SHA256::add` for raw bytes, the savings per event is ~100-150ns. For 9600 events across 3200 TXs: ~1-1.5ms total unparallelized. With 8 threads: ~125-190μs wall-clock savings. Against a ~2-3s ledger close, this is ~0.005-0.01%.

3. **Pseudocode field order is wrong**: The hypothesis shows events first, then return value. The XDR struct definition serializes `returnValue` first, then `events`. The correct raw-bytes approach would be: `SHA256::add(raw_return_value_bytes)` → `SHA256::add(uint32_be(event_count))` → loop `SHA256::add(raw_event_bytes)`.

4. **Correctness is achievable**: The hash is computed before `setEvents` (which may prepend protocol-23 reconciliation events), so it uses only the original Rust-provided events. The raw bytes from Rust are canonical XDR. Manual XDR framing (just a uint32 length prefix for the vector) is straightforward.

5. **Severity downgrade**: The hypothesis claims Low severity. The actual savings (~0.005-0.01% of ledger close time) is well below the 5% threshold for Low. This is Informational — a real finding with negligible practical impact.

### PoC Guidance

- **Target code**: `src/transactions/InvokeHostFunctionOpFrame.cpp` — `finalizeSuccess` method (line 816-829). Replace `xdrSha256(success)` with a manual SHA256 construction that hashes raw bytes from `out.result_value.data` and `out.contract_events[i].data`.
- **Change description**: Build a `SHA256` instance, add `out.result_value.data` (raw XDR of return value SCVal), add `uint32_be(out.contract_events.size())` (vector length prefix), then loop adding each `out.contract_events[i].data` (raw XDR of ContractEvent). Call `finish()` to get the hash. This replaces the `xdrSha256(success)` call while still decoding events separately for `setEvents`.
- **Correctness check**: Existing tests for `InvokeHostFunctionOpFrame` (InvokeHostFunctionTests) and the parallel apply tests should pass unchanged, since the hash value is identical.
- **Benchmark focus**: The savings (~0.005% of ledger close time) is below measurable benchmark noise. A microbenchmark comparing `xdrSha256(success)` vs. raw-bytes hashing per-event would be needed to confirm the improvement, but it will not show up in apply-load benchmarks.
