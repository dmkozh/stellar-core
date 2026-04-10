# H012: Custom Hash Function for Verification Cache Avoids SipHash+Mutex

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: Informational
**Impact**: eliminates nested mutex acquisition in verification cache lookups
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

The signature verification cache (`gVerifySigCache`) uses BLAKE2 hashes as keys. Since BLAKE2 output is uniformly distributed, the `std::unordered_map` hash function for these keys should be a trivial identity-like hash (e.g., reinterpreting the first 8 bytes as `size_t`), not a full SipHash-2-4 computation through a mutex-protected global key.

## Mechanism

`gVerifySigCache` is a `RandomEvictionCache<Hash, bool>` (SecretKey.cpp:47) whose internal `unordered_map` defaults to `std::hash<Hash>` = `std::hash<uint256>`. This hash function (HashOfHash.cpp:12-17) calls `shortHash::computeHash(ByteSlice(x.data(), 8))`, which acquires `gKeyMutex`, runs SipHash-2-4 on 8 bytes, and releases the mutex. This happens for every `find()` and `insert()` on the map.

Since the cache keys are BLAKE2 hashes (already cryptographically uniform), applying SipHash is redundant — the first 8 bytes of a BLAKE2 output already have excellent distribution for hash table use. Providing a custom hash function `struct Blake2KeyHash { size_t operator()(Hash const& h) const noexcept { size_t r; memcpy(&r, h.data(), sizeof(r)); return r; } }` would eliminate ~2 `gKeyMutex` acquisitions per `verifySig` call (one per `find` in hit path; one for `find` + one for `insert` on miss path).

## Trigger

Run T=8 apply-load. Profile lock contention on `gKeyMutex` within the `verifySig` code path. With 8 threads potentially accessing the verification cache (during tx set validation), the nested lock `gVerifySigCacheMutex` → `gKeyMutex` creates a lock ordering dependency and extends the outer critical section.

## Target Code

- `src/crypto/SecretKey.cpp:47` — gVerifySigCache uses default std::hash<Hash>
- `src/util/HashOfHash.cpp:12-17` — std::hash<uint256> calls shortHash::computeHash
- `src/crypto/ShortHash.cpp:61-72` — computeHash acquires gKeyMutex for full SipHash computation
- `src/util/RandomEvictionCache.h:56` — MapType uses Hash template parameter for unordered_map

## Evidence

The nested lock pattern is verifiable: `verifySig` holds `gVerifySigCacheMutex` (line 462), calls `exists()` which calls `mValueMap.find()` which calls `std::hash<uint256>()` which calls `shortHash::computeHash` which acquires `gKeyMutex` (ShortHash.cpp:64). This means every cache operation has a nested mutex acquisition. For BLAKE2 cache keys, the SipHash computation is pure overhead — the input is already uniformly distributed.

## Anti-Evidence

H006 (shortHash global mutex) was benchmarked via a PoC that removed `gKeyMutex` from ALL shortHash calls. The result showed no measurable improvement and even regressions on some scenarios. If removing the mutex globally had no effect, providing a custom hash for just the verification cache (which eliminates the mutex for that one data structure) would have even less effect. The verification cache is only actively used during the pre-apply validation phase, which for the benchmark is single-threaded (no contention). Soroban parallel apply doesn't touch the verification cache at all.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PARTIAL — related to H006 (shortHash mutex) but targets a different mechanism (hash function choice vs mutex removal)

### Why It Failed

H006 already tested the broader optimization of removing `gKeyMutex` from all shortHash calls. Its PoC passed all tests but showed no measurable benchmark improvement, with some scenarios actually regressing. This narrower optimization (custom hash only for the verification cache) would produce strictly less savings than H006's global mutex removal, since it only affects one data structure. Furthermore, the verification cache is accessed single-threaded during the benchmark's pre-apply phase, so there is no contention to eliminate. The optimization is theoretically sound but empirically irrelevant for the apply-load benchmark.

### Lesson Learned

When a broader optimization targeting the same mechanism has already been benchmarked and rejected, narrower variants targeting the same mechanism are unlikely to succeed. The shortHash mutex is confirmed to be a non-bottleneck for the apply-load benchmark, regardless of whether it's removed globally (H006) or bypassed for specific data structures.
