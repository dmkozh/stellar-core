# H010: verifySigCacheKey Uses Incremental BLAKE2 When One-Shot Suffices

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: Informational
**Impact**: reduced per-verification hashing overhead
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

Computing the signature verification cache key for a 128-byte input (32-byte public key + 64-byte signature + 32-byte message hash) should use the most efficient hashing approach available. For small, fixed-size inputs that can be assembled contiguously on the stack, a one-shot `crypto_generichash` call is more efficient than creating an incremental BLAKE2 state object, performing 3 separate update calls, and finalizing.

## Mechanism

`verifySigCacheKey` (SecretKey.cpp:55-66) creates a `BLAKE2` object on the stack, which calls `crypto_generichash_init` to initialize a 361-byte `crypto_generichash_state` structure. It then performs 3 separate `add()` calls (each with a `ZoneScoped` guard when Tracy is enabled) via `crypto_generichash_update`, and finally calls `crypto_generichash_final`. For a total of 128 bytes of input (well under BLAKE2b's 128-byte block size), all the data fits in a single compression block. A one-shot approach — copy the 3 inputs into a 128-byte stack buffer, then call `crypto_generichash` once — would avoid the incremental state machine overhead: no init/finalize ceremony, no 3 separate update dispatch calls, and a smaller stack footprint (128 bytes vs 361+ bytes for the state).

The savings per call are estimated at ~50-150ns (removing init/finalize overhead and reducing function call dispatch from 5 calls to 1). With the signature cache pre-warmed in the benchmark, `verifySigCacheKey` is still called for every signature verification (it runs BEFORE the cache check), but the total savings across ~9600 calls per ledger close (3200 txs × 3 verifications) would be ~0.5-1.4ms — below the apply-load benchmark measurement threshold.

## Trigger

Run any apply-load scenario. `verifySigCacheKey` is called for every `verifySig` invocation regardless of cache hit/miss. Profile `crypto_generichash_init` and `crypto_generichash_update` call frequency within the signature verification path. Compare against a build using one-shot `crypto_generichash` with a stack-assembled 128-byte buffer.

## Target Code

- `src/crypto/SecretKey.cpp:55-66` — verifySigCacheKey creates BLAKE2 incrementally with 3 add() calls
- `src/crypto/BLAKE2.cpp:31-45` — BLAKE2 constructor calls crypto_generichash_init (361-byte state)
- `src/crypto/BLAKE2.cpp:48-58` — add() calls crypto_generichash_update with ZoneScoped
- `src/crypto/BLAKE2.cpp:62-74` — finish() calls crypto_generichash_final
- `src/crypto/BLAKE2.cpp:16-28` — one-shot blake2() function wrapping crypto_generichash

## Evidence

The one-shot `blake2()` function already exists (BLAKE2.cpp:16-28) and wraps `crypto_generichash` for contiguous inputs. The current code creates an incremental hasher for what is effectively a fixed-size, small input. The proposed fix:
```cpp
static Hash
verifySigCacheKey(PublicKey const& key, Signature const& signature,
                  ByteSlice const& bin)
{
    releaseAssert(key.type() == PUBLIC_KEY_TYPE_ED25519);
    // 32-byte pubkey + 64-byte signature + 32-byte message = 128 bytes max
    unsigned char buf[128];
    size_t off = 0;
    memcpy(buf + off, key.ed25519().data(), 32); off += 32;
    memcpy(buf + off, signature.data(), signature.size()); off += signature.size();
    memcpy(buf + off, bin.data(), bin.size()); off += bin.size();
    return blake2(ByteSlice(buf, off));
}
```
This replaces 5 libsodium function calls (init + 3 update + final) with 1 (crypto_generichash), 3 memcpy operations for buffer assembly, and avoids allocating a 361-byte state on the stack.

## Anti-Evidence

The actual BLAKE2 compression dominates the cost (~200-400ns for 128 bytes), so the init/finalize overhead (~50-150ns) is a minority. The total savings of ~0.5-1.4ms per ledger close is <1% of benchmark time. Four prior crypto hypotheses (H005-H008) were all rejected at the benchmark level, establishing that crypto operations are not a significant contributor to apply-load benchmark time. The signature verification cache is pre-warmed during tx generation, and Soroban parallel apply skips signature verification entirely.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full `verifySig` → `verifySigCacheKey` path in SecretKey.cpp:447-495. Confirmed that `verifySigCacheKey` (lines 55-66) creates a `BLAKE2` incremental hasher on the stack, calls `add()` three times (32-byte ed25519 key, 64-byte signature, variable-length message), then `finish()`. The libsodium one-shot `blake2b()` (blake2b-ref.c:335-369) internally performs the same `blake2b_init` + `blake2b_update` + `blake2b_final` sequence, but with a single update call. The actual `crypto_generichash_state` is 384 bytes (not 361 as hypothesized) with 64-byte alignment. The core BLAKE2b compression is identical in both paths — savings come only from reduced C++ function call dispatch and Tracy overhead.

### Code Paths Examined

- `src/crypto/SecretKey.cpp:55-66` — `verifySigCacheKey`: creates `BLAKE2`, 3× `add()`, `finish()` — confirmed incremental pattern
- `src/crypto/SecretKey.cpp:447-495` — `verifySig`: calls `verifySigCacheKey` on every invocation before cache check
- `src/crypto/BLAKE2.cpp:31-44` — `BLAKE2()` constructor: calls `crypto_generichash_init` on 384-byte state
- `src/crypto/BLAKE2.cpp:48-58` — `add()`: ZoneScoped + `crypto_generichash_update` + mFinished check
- `src/crypto/BLAKE2.cpp:62-74` — `finish()`: mFinished check + `crypto_generichash_final`
- `src/crypto/BLAKE2.cpp:16-28` — one-shot `blake2()`: single `crypto_generichash` call
- `lib/libsodium/.../blake2b-ref.c:335-369` — one-shot `blake2b()`: init + single update + final (same compression, fewer calls)
- `lib/libsodium/.../generichash_blake2b.c:46-66` — `crypto_generichash_blake2b_init`: parameter validation + `blake2b_init`
- `src/transactions/SignatureUtils.cpp:38-46` — primary tx verification call site: `bin` is `Hash` (32 bytes), total input = 128 bytes
- `src/transactions/SignatureUtils.cpp:48-61` — signed-payload path: `bin` is `signedPayload.payload` (up to 64 bytes), total up to 160 bytes

### Findings

**The inefficiency is real but minimal.** The incremental BLAKE2 path for `verifySigCacheKey` involves:
- 6 C++ function calls (constructor, 3× add, finish, plus internal reset) vs. 1 (blake2)
- 3 Tracy ZoneScoped guards in add() vs. 1 in blake2()
- 3 `crypto_generichash_update` dispatch calls vs. 1 `blake2b_update` call
- mFinished flag checks in add() and finish()

The underlying BLAKE2b compression is identical — 128 bytes fits in one block regardless of how many update calls deliver it. Realistic savings: ~30-50ns per call without Tracy, ~50-100ns with Tracy enabled.

**Correctness issue in proposed fix:** The hypothesis proposes a 128-byte stack buffer, but `bin` is a `ByteSlice` of arbitrary length. While the hot path (transaction signature verification via `SignatureUtils::verify`) always passes a 32-byte `Hash`, other callers pass larger messages:
- `verifyEd25519SignedPayload`: `signedPayload.payload` — up to 64 bytes (total: 160 bytes)
- `HerderImpl::verifySCPSignature`: `xdr_to_opaque(...)` — variable, potentially hundreds of bytes
- `Peer::recvSCPMessage`: `xdr_to_opaque(...)` — variable

A correct fix must either: (a) use a larger buffer with fallback to incremental for oversized messages, or (b) limit the optimization to a known-size fast path.

**Impact assessment:** At ~9600 calls per SAC benchmark ledger close (pre-warmed cache, so these are all cache hits returning quickly), saving ~50ns each yields ~0.48ms — well below the ~200ms benchmark measurement noise floor. The optimization is real but unmeasurable in apply-load benchmarks.

### PoC Guidance

- **Target code**: `src/crypto/SecretKey.cpp:55-66` — replace `verifySigCacheKey` implementation
- **Change description**: Use a 256-byte stack buffer to assemble `pubkey || signature || message`, then call `blake2(ByteSlice(buf, off))`. For the unlikely case where total exceeds 256 bytes, fall back to the current incremental approach. This covers all current call sites (max 160 bytes for signed-payload path).
- **Correctness check**: `CryptoTests` test suite covers `verifySig` directly (lines 289, 292, 296, 638, 1643 of CryptoTests.cpp). Run: `./src/stellar-core test --ll fatal -r simple --abort --disable-dots "[crypto]"`
- **Benchmark focus**: This is Informational severity — savings (~0.5ms per ledger close) are below apply-load measurement threshold. A microbenchmark comparing one-shot vs incremental BLAKE2 for 128-byte inputs would confirm the per-call savings (~50-100ns).
