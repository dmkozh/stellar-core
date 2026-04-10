# H015: Replace std::function in modifyInScope with Template-Based Callback

**Date**: 2025-07-22
**Subsystem**: transaction-ledger (LedgerEntryScope)
**Severity**: Informational
**Impact**: Reduced type-erasure overhead in hot path
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

The `modifyInScope` and `scopeModifyEntry` methods should invoke their callback
with zero overhead beyond a direct function call. The callback pattern should
allow the compiler to inline the lambda body.

## Mechanism

`ScopedLedgerEntry::modifyInScope` (LedgerEntryScope.cpp:153-157) and
`ScopedLedgerEntryOpt::modifyOptionalEntry` take a
`std::function<void(LedgerEntry&)>` parameter. `std::function` involves:

1. Type erasure overhead (~20-50ns per invocation for virtual dispatch)
2. Potential heap allocation if the lambda capture exceeds the Small Buffer
   Optimization (SBO) size (typically 24-32 bytes on x86-64)
3. Prevention of inlining — the compiler cannot see through the type-erased
   call to inline the lambda body

These methods are called in per-entry hot paths:
- `TxParallelApplyLedgerState::upsertEntry` (line 918) — per modified entry
- `ThreadParallelApplyLedgerState::flushRoTTLBumpsInTxWriteFootprint` (line 648)
- `commitChangeFromSuccessfulTx` (various entry updates)

For 3200 txs × ~5 entries = ~16,000 calls. The fix: template the callback
parameter to allow direct invocation and inlining.

## Trigger

Any parallel apply with entry modifications. In the apply-load benchmark:
~16,000+ `modifyInScope` calls per ledger close.

## Target Code

- `src/ledger/LedgerEntryScope.cpp:modifyInScope:153-157` — takes std::function
- `src/ledger/LedgerEntryScope.cpp:scopeModifyEntry:355-367` — takes std::function
- `src/ledger/LedgerEntryScope.cpp:scopeModifyOptionalEntry:383-396` — takes std::function
- `src/ledger/LedgerEntryScope.h:271-272,311-312` — declarations

## Evidence

1. `std::function` is passed by value in `scopeModifyEntry` (line 358), which copies the function object on every call (even with SBO, this involves vtable dispatch).
2. The scope system is template-parameterized on `StaticLedgerEntryScope` but the callback is type-erased, preventing cross-function inlining.
3. The callbacks are typically simple lambdas (1-3 lines) that would benefit greatly from inlining.

## Anti-Evidence

1. Most lambdas used with `modifyInScope` have small captures (1-2 references), fitting within SBO. No heap allocation occurs.
2. The virtual dispatch overhead is ~20-50ns per call. For 16,000 calls: ~0.3-0.8ms total. This is well under 1% of ledger close time.
3. Templating the callback would require making `modifyInScope` and `scopeModifyEntry` header-only template methods, increasing compilation time and code size. The LedgerEntryScope system is already heavily templated.
4. The scope checking (mScopeID comparison) in `scopeModifyEntry` (lines 360-365) is the primary purpose of these methods. Removing type erasure doesn't eliminate the scope check overhead.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-22
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The total overhead from `std::function` type erasure is ~0.3-0.8ms for ~16,000
calls per ledger close. This represents <1% of a 50-100ms close time, well
below the Informational threshold (1-3%). The lambdas used with `modifyInScope`
have small captures that fit within SBO (no heap allocation), and the dominant
cost in `scopeModifyEntry` is the scope ID comparison, not the callback
dispatch. Additionally, templating these methods would significantly increase
code complexity in an already heavily-templated system for negligible
performance gain.

### Lesson Learned

When `std::function` is used with small-capture lambdas that fit within SBO
(~24-32 bytes), the overhead per call is ~20-50ns — dominated by virtual
dispatch, not allocation. This is only significant when call counts reach
hundreds of thousands per ledger. For ~16K calls, the total is sub-millisecond.
