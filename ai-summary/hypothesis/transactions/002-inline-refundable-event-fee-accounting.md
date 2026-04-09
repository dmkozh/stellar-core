# H002: Inline Refundable Event-Fee Accounting Instead of Recomputing Full Tx Fees

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: per-tx post-host CPU / avoidable C++↔Rust fee bridge calls
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

After Soroban host execution, refundable-fee tracking should update the
refundable component using only the dynamic quantities that actually changed:
contract-event bytes and rent. The hot path should not rerun the full
transaction resource-fee calculation when the non-refundable portion was
already charged during `commonPreApply`.

## Mechanism

`RefundableFeeTracker::consumeRefundableSorobanResources` currently calls
`TransactionFrame::computeSorobanResourceFee` and then uses only
`consumedFee.refundable_fee`. But the Rust fee implementation computes
`refundable_fee` exclusively from `contract_events_size_bytes`; all other tx
resource components contribute only to `non_refundable_fee`, which
`commonPreApply` already handled. That means every successful invoke-host call
currently pays for an unnecessary full fee recomputation — including tx-size
recovery and a Rust bridge call — even though the needed result is just
`ceil(event_bytes / 1024) * fee_per_contract_event_1kb` plus the separately
tracked rent fee.

## Trigger

Run `scripts/run_apply_load_matrix.py` and profile successful Soroban txs,
especially batched SAC (`sac,TX=6400,T=8`) where each tx emits many events.
Expect to see `consumeRefundableSorobanResources` on every success even though
the dynamic post-host input is just emitted event bytes and `out.rent_fee`.

## Target Code

- `src/transactions/MutableTransactionResult.cpp:RefundableFeeTracker::consumeRefundableSorobanResources:39-79` — recomputes full Soroban resource fees to recover the refundable part
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::consumeRefundableResources:757-768` — calls the slow path after every successful host invocation
- `src/transactions/TransactionFrame.cpp:TransactionFrame::computeSorobanResourceFee:1159-1195` — rebuilds a full `CxxTransactionResources` and crosses the Rust fee bridge
- `src/ledger/NetworkConfig.cpp:SorobanNetworkConfig::rustBridgeFeeConfiguration:2849-2866` — already exposes the event-fee rate needed for a direct C++ fast path

## Evidence

The fee configuration passed over the bridge contains an explicit
`fee_per_contract_event_1kb`, and the protocol fee code computes
`refundable_fee = events_fee` with `events_fee` derived solely from
`contract_events_size_bytes` via `compute_fee_per_increment(...)`. The current
C++ path therefore recomputes instruction, read-entry, write-entry,
historical-byte, and tx-size fee components that cannot affect the refundable
result it actually consumes.

## Anti-Evidence

Any fast path must remain protocol-aware: if a future protocol makes the
refundable component depend on more than event bytes, the optimization needs a
version gate or fallback to the existing bridge call. The improvement is also
much smaller for `RestoreFootprint` / `ExtendFootprintTTL`, which call the same
tracker with zero event bytes.
