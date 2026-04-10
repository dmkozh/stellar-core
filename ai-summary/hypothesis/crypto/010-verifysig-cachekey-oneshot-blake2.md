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
