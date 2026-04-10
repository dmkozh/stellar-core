# H012: Eliminate Redundant `xdrSha256(header)` in `storePersistentStateAndLedgerHeaderInDB` and `buildLedgerState`

**Date**: 2026-04-10
**Subsystem**: ledger (LedgerManagerImpl)
**Severity**: Informational
**Impact**: Eliminate one redundant SHA-256 hash computation per ledger close
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The SHA-256 hash of the `LedgerHeader` should be computed at most once during
the seal-and-store phase. `storePersistentStateAndLedgerHeaderInDB` (line 2901)
computes `xdrSha256(header)` as an assertion check, and then `buildLedgerState`
(line 2124) computes `xdrSha256(header)` again to produce the LCL hash. The
second computation should reuse the result from the first.

## Mechanism

In `sealLedgerTxnAndStoreInBucketsAndDB` (lines 3097-3104), the unseal
callback calls `storePersistentStateAndLedgerHeaderInDB(lh, true)` which
executes `releaseAssert(!isZero(xdrSha256(header)))` at line 2901 — one
SHA-256 computation. Then `advanceApplySnapshotAndMakeLedgerState(lh, has, ...)`
is called, which invokes `buildLedgerState` at line 2137, which computes
`lcl.hash = xdrSha256(header)` at line 2124 — a second SHA-256 computation
on the same header data.

The LedgerHeader is ~200-300 bytes when XDR-serialized. SHA-256 on this
input takes ~1-2μs. The redundant computation wastes ~1-2μs per ledger close.

## Trigger

Profile `xdrSha256` calls during `sealLedgerTxnAndStoreInBucketsAndDB`.
Two calls should be visible on the same header data within the same
function flow.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:storePersistentStateAndLedgerHeaderInDB:2901` — `releaseAssert(!isZero(xdrSha256(header)))`
- `src/ledger/LedgerManagerImpl.cpp:buildLedgerState:2124` — `lcl.hash = xdrSha256(header)`

## Evidence

- Both calls compute SHA-256 on the same `LedgerHeader const&` data
- Both are called within the same `unsealHeader` callback (lines 3097-3104)
- The header is not modified between the two calls

## Anti-Evidence

- SHA-256 on ~300 bytes is ~1-2μs — negligible compared to total close time (~100ms-2s)
- The assertion at line 2901 serves a different purpose (validity check) than the hash at line 2124 (LCL computation)
- Caching the hash result across function boundaries adds API complexity for microsecond savings

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The redundant SHA-256 computation saves ~1-2μs per ledger close. With total
close time in the 100ms-2s range, this is <0.002% improvement — far below
the Informational threshold of 1%. The added complexity of threading a cached
hash through the function call chain is not justified by such marginal savings.

### Lesson Learned

SHA-256 on small inputs (<1KB) completes in low microseconds and is not worth
optimizing unless called thousands of times per ledger (as with `getTTLKey`).
Per-ledger SHA-256 computations on headers, BucketList hashes, etc. are
negligible relative to the overall close time.
