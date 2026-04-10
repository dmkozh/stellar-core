# H025: Batch Fee Refunds in processPostTxSetApply

**Date**: 2025-07-14
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: fee refund path optimization
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`processPostTxSetApply` should batch all 6400 Soroban fee refund operations
into a single LedgerTxn pass instead of creating 6400 separate nested
LedgerTxn instances (one per transaction). This would eliminate 6399
LedgerTxn constructor/destructor cycles and 6399 commit operations.

## Mechanism

In `LedgerManagerImpl::processPostTxSetApply` (lines 2838-2884), each
Soroban transaction gets its own `LedgerTxn ltxInner(ltx)` to process the
fee refund. Each iteration involves:
- LedgerTxn::Impl construction (~150ns: heap alloc + map init)
- Load fee source account from parent entry map (~500ns)
- Modify balance for refund (~50ns)
- Generate fee event (~200ns)
- getChanges() for meta capture (~100ns if meta enabled)
- commit() back to parent (~500ns: merge entries)
Total: ~1.5-2µs per tx. For 6400 txs: ~10-13ms serial.

The proposed batch approach would load all unique fee source accounts once
in a single LedgerTxn, apply all refunds, and commit once.

## Trigger

Run apply-load benchmark with SAC TX=6400, T=8. Profile
`processPostTxSetApply` to measure total time and per-iteration overhead.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:processPostTxSetApply:2838-2884` —
  serial loop creating nested LedgerTxn per tx
- `src/transactions/TransactionFrame.cpp:processRefund:2592-2615` —
  refund computation per tx

## Evidence

- 6400 LedgerTxn create/commit cycles are visible in the serial loop
- Each cycle involves heap allocation for Impl + map operations

## Anti-Evidence

- LedgerTxn has a "key is active" constraint: loading the same key twice
  in a single LedgerTxn throws. Multiple txs sharing the same fee source
  would need the key to be loaded, modified, deactivated, and re-loaded.
- The current nested-LedgerTxn pattern handles this automatically via scope.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (distinct from fail 003
which targets flattening the LedgerTxn nesting depth, not batching refunds)

### Why It Failed

The `LedgerTxn::load()` method throws if a key is already in `mActive`
(line 1888-1891 in LedgerTxn.cpp). When multiple Soroban txs share the
same fee source account, the batching approach would need to
load → modify → deactivate → reload the same key multiple times within one
LedgerTxn. This requires letting the `LedgerTxnEntry` go out of scope
between each refund, which effectively recreates the same nested scoping
pattern as the current code.

Furthermore, the total overhead is only ~10-13ms (1.6-2.1% of close time),
well below the 5% Low severity threshold. Even a perfect elimination of all
overhead would save <2% — insufficient to register as measurable improvement.

### Lesson Learned

LedgerTxn's active-entry constraint prevents batching modifications to
shared keys. Any optimization of serial LedgerTxn loops must work within
this constraint, which limits batching to independent (non-overlapping) key
sets. For the fee refund path, most txs share fee source accounts across the
set, making batching impractical.
