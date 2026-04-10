# H009: verifySig Double Hash Map Lookup — Use maybeGet Instead of exists()+get()

**Date**: 2026-04-10
**Subsystem**: crypto
**Severity**: Informational
**Impact**: reduced critical section duration on signature cache mutex
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When checking the signature verification cache for a hit, the code should perform a single hash map lookup to find the entry, read its value, and update the access timestamp. This minimizes the time spent holding `gVerifySigCacheMutex` and reduces per-verification overhead.

## Mechanism

`PubKeyUtils::verifySig` (SecretKey.cpp:462-470) checks the cache with `gVerifySigCache.exists(cacheKey)` and then, on hit, retrieves the value with `gVerifySigCache.get(cacheKey)`. Both `exists()` and `get()` independently call `mValueMap.find(k)` (RandomEvictionCache.h:162,231), performing two hash map lookups for the same key. Each lookup invokes `std::hash<uint256>` → `shortHash::computeHash(ByteSlice(x.data(), 8))` (HashOfHash.cpp:13-16), which acquires `gKeyMutex` and runs SipHash-2-4. This means each cache HIT path performs 2 hash map lookups instead of 1, with 2 nested `gKeyMutex` acquisitions inside the `gVerifySigCacheMutex` critical section.

The `RandomEvictionCache` already provides `maybeGet(k)` (line 210-225) which does a single `find()`, updates `mLastAccess`, counts the hit, and returns a pointer to the value — all in one lookup.

## Trigger

Run any apply-load scenario. The signature cache is pre-warmed during tx generation, so every signature verification during the measured ledger close hits the cache. Each hit currently does 2 hash map lookups instead of 1. With ~3200 SAC transactions × 3 verification passes × ~66% hit rate = ~6400 redundant hash map lookups per ledger close. However, each saved lookup is ~50-100ns, yielding ~0.3-0.6ms total savings per ledger close — below the measurement threshold of the apply-load benchmark.

## Target Code

- `src/crypto/SecretKey.cpp:462-470` — verifySig cache hit path uses exists() then get()
- `src/util/RandomEvictionCache.h:159-174` — exists() does mValueMap.find()
- `src/util/RandomEvictionCache.h:228-237` — get() calls maybeGet() which does mValueMap.find() again
- `src/util/RandomEvictionCache.h:210-225` — maybeGet() does single find + hit counting + access update
- `src/util/HashOfHash.cpp:12-17` — std::hash<uint256> calls shortHash::computeHash per lookup

## Evidence

The double-lookup pattern is clear in the code: `exists()` at line 463 performs `mValueMap.find(k)`, discards the iterator, and returns a bool. Then `get()` at line 468 calls `maybeGet()` which performs `mValueMap.find(k)` again to retrieve the value. The `maybeGet()` API exists specifically to avoid this pattern, doing find+access-update+hit-counting in one call. The fix is mechanical:
```cpp
// Before (2 lookups):
if (gVerifySigCache.exists(cacheKey))
{
    ++gVerifyCacheHit;
    return {gVerifySigCache.get(cacheKey), VerifySigCacheLookupResult::HIT};
}
// After (1 lookup):
if (auto* cached = gVerifySigCache.maybeGet(cacheKey))
{
    ++gVerifyCacheHit;
    return {*cached, VerifySigCacheLookupResult::HIT};
}
```

## Anti-Evidence

The apply-load benchmark pre-warms the signature cache during tx generation (ApplyLoad.cpp:2138-2149), and Soroban transactions use `parallelApply` which skips signature verification entirely. The only signature verification during the measured benchmark time is for classic transactions in the sequential phase. The per-lookup savings (~50-100ns) are far below the noise floor of the benchmark. Four previous crypto optimization hypotheses (H005-H008) were all rejected at the benchmark level because crypto operations constitute <1% of the measured apply-load time, dominated by Soroban host execution.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete `verifySig` cache-hit path: `PubKeyUtils::verifySig` (SecretKey.cpp:461-470) acquires `gVerifySigCacheMutex`, calls `exists()` which performs `mValueMap.find(cacheKey)` (RandomEvictionCache.h:162) — this triggers `std::hash<uint256>` (HashOfHash.cpp:12-17) which acquires `gKeyMutex` and runs SipHash. On hit, it then calls `get()` → `maybeGet()` which performs a second `mValueMap.find(cacheKey)` (RandomEvictionCache.h:212) — triggering the same hash+mutex again. The `maybeGet()` API (lines 210-225) is purpose-built to replace this exact pattern with a single lookup. The fix is mechanical and preserves all correctness, counter semantics, and LRU-access-timestamp updates.

### Code Paths Examined

- `src/crypto/SecretKey.cpp:461-470` — `verifySig` under `gVerifySigCacheMutex`: calls `exists(cacheKey)` then `get(cacheKey)` on cache hit. Two separate lookups within the critical section.
- `src/util/RandomEvictionCache.h:159-174` — `exists()` does `mValueMap.find(k)`, returns `bool`. On miss, increments `mCounters.mMisses`. Does NOT update `mLastAccess` or count hits.
- `src/util/RandomEvictionCache.h:228-237` — `get()` calls `maybeGet(k)` internally, which does another `mValueMap.find(k)`. Updates `mLastAccess` and counts `mCounters.mHits`.
- `src/util/RandomEvictionCache.h:210-225` — `maybeGet()` performs a single `find()`, updates `mLastAccess`, counts `mHits` on hit, counts `mMisses` on miss, returns `V*` or `nullptr`.
- `src/util/HashOfHash.cpp:12-17` — `std::hash<uint256>` calls `shortHash::computeHash(ByteSlice(x.data(), 8))` — acquires `gKeyMutex`, runs SipHash-2-4. This is invoked by each `mValueMap.find()`.
- `src/crypto/ShortHash.cpp:61-72` — `computeHash()` acquires `gKeyMutex` (line 64), sets `gHaveHashed = true`, runs `crypto_shorthash`, returns result.
- `src/util/RandomEvictionCache.h:164-168` — Comment in `exists()` itself recommends: "Or use the maybeGet interface, which will save you a second hash lookup and provides a less-cumbersome interface."

### Findings

The double-lookup inefficiency is confirmed and the fix is correct:

1. **The inefficiency exists**: Every cache hit performs two `mValueMap.find()` calls with identical keys. Each `find()` computes `std::hash<uint256>` which calls `shortHash::computeHash` under `gKeyMutex`. This means the cache-hit path acquires `gKeyMutex` twice within the outer `gVerifySigCacheMutex` critical section.

2. **The fix preserves correctness**: Switching from `exists()+get()` to `maybeGet()` produces identical counter behavior — `maybeGet()` increments `mCounters.mHits` on hit (same as `get()→maybeGet()` does currently) and `mCounters.mMisses` on miss (same as `exists()` with `countMisses=true`). The `mLastAccess` update (LRU eviction timestamp) occurs in `maybeGet()` in both patterns. The external `gVerifyCacheHit`/`gVerifyCacheMiss` counters are managed by `verifySig` itself and unaffected.

3. **Impact is real but sub-threshold**: Per the hypothesis's own analysis, ~6400 redundant lookups at ~75ns each ≈ 0.5ms per ledger close, well under the benchmark noise floor. The `gVerifySigCacheMutex` already serializes access, so there's no contention benefit — only a shortened critical section.

4. **Code quality improvement**: The `RandomEvictionCache` comments (lines 167-168) explicitly recommend `maybeGet` over the `exists()+get()` pattern. This is a clean code improvement independent of performance.

### PoC Guidance

- **Target code**: `src/crypto/SecretKey.cpp:461-470` — replace the `exists()`+`get()` pattern with `maybeGet()`
- **Change description**: Replace lines 463-470 with:
  ```cpp
  if (auto* cached = gVerifySigCache.maybeGet(cacheKey))
  {
      ++gVerifyCacheHit;
      std::string hitStr("hit");
      ZoneText(hitStr.c_str(), hitStr.size());
      return {*cached, VerifySigCacheLookupResult::HIT};
  }
  ```
- **Correctness check**: Run `"[tx]"` test tag — all signature verification tests pass through this path. Also run `"SignatureChecker"` and `"verifySig cache"` (or equivalent) test names.
- **Benchmark focus**: This is Informational severity — the benchmark is not expected to show measurable improvement. The primary value is code quality (eliminating a known anti-pattern flagged in the code's own comments).
