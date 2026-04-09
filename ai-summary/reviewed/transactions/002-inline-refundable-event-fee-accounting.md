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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full path from `consumeRefundableSorobanResources` through `computeSorobanResourceFee` into the Rust FFI bridge (`soroban_invoke::compute_transaction_resource_fee` → `get_host_module_for_protocol` → `compute_transaction_resource_fee`). Confirmed that the Rust implementation computes 7 fee components (instructions, read entries, write entries, read bytes, write bytes, historical, bandwidth) but `refundable_fee` is set exclusively to `events_fee = compute_fee_per_increment(contract_events_size_bytes, fee_per_contract_event_1kb, 1024)`. This formula is identical across all protocol versions (p21–p26). The C++ side already exposes `SorobanNetworkConfig::feeContractEventsSize1KB()`, making an inline replacement trivial.

### Code Paths Examined

- `src/transactions/MutableTransactionResult.cpp:39-80` (`consumeRefundableSorobanResources`) — Builds full `CxxTransactionResources` via `tx.getResources(false, protocolVersion).getVal(TX_BYTE_SIZE)` (which calls `getSize()` → `xdr_size(mEnvelope)`), then calls `computeSorobanResourceFee` which crosses the Rust bridge, only to extract `.refundable_fee`.
- `src/transactions/TransactionFrame.cpp:1158-1195` (`computeSorobanResourceFee`) — Populates 8 fields on `CxxTransactionResources`, calls `rustBridgeFeeConfiguration()` to copy 8 fee config fields, then crosses FFI to Rust.
- `src/rust/src/soroban_invoke.rs:63-74` — Rust entry point: calls `get_host_module_for_protocol` (linear scan of HOST_MODULES array), then dispatches to the protocol-specific `compute_transaction_resource_fee`.
- `src/rust/soroban/p26/soroban-env-host/src/fees.rs:149-205` — Computes 7 fee components (compute, read_entry, write_entry, read_bytes, write_bytes, historical, bandwidth, events) but returns `(non_refundable_fee, refundable_fee)` where `refundable_fee = events_fee`.
- `src/rust/soroban/p26/soroban-env-host/src/fees.rs:418-421` (`compute_fee_per_increment`) — `div_ceil(resource_val * fee_rate, increment)` — trivial integer arithmetic.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:757-768` (`consumeRefundableResources`) — Called from `doApply()` (line 911) after every successful host invocation, in both sequential and parallel paths.
- `src/transactions/ExtendFootprintTTLOpFrame.cpp:187-189` — Calls with `contractEventSizeBytes=0`, making the entire bridge call produce `refundable_fee=0`.
- `src/transactions/RestoreFootprintOpFrame.cpp:235-237` — Same as ExtendFootprintTTL: passes 0 event bytes.
- `src/ledger/NetworkConfig.h:405` — `feeContractEventsSize1KB()` accessor is already public on `SorobanNetworkConfig`.
- `src/rust/soroban/p21-p26/soroban-env-host/src/fees.rs` — Confirmed `refundable_fee = events_fee` across ALL protocol versions (p21–p26), making this a stable invariant.

### Findings

**The inefficiency is confirmed:** `consumeRefundableSorobanResources` pays for a full Rust bridge round-trip including:
1. `getResources()` → `getSize()` → `xdr_size(mEnvelope)` XDR tree walk (~2–5µs for SAC batch envelopes)
2. `CxxTransactionResources` construction (8 fields, most unused)
3. `rustBridgeFeeConfiguration()` (8 field copies)
4. FFI crossing + `get_host_module_for_protocol` linear scan
5. 7 fee component computations in Rust, of which 6 are discarded

The entire result could be computed in C++ as:
```cpp
int64_t eventFeeRate = sorobanConfig.feeContractEventsSize1KB();
int64_t eventsSize = static_cast<int64_t>(mConsumedContractEventsSizeBytes);
int64_t eventsFee = (eventsSize * eventFeeRate + 1023) / 1024; // div_ceil
```

**Severity downgraded to Informational.** While the inefficiency is real and the fix is trivially correct, the per-tx overhead (~3–7µs) runs in the parallel apply phase (v23+), where it is diluted across threads. For SAC TX=6400, T=8: ~800 txs/thread × 5µs ≈ 4ms per thread, versus a parallel phase dominated by host invocation (~100–500µs/tx for SAC). The wall-clock impact is <1% of total close time, well below the 5% threshold for Low severity. For ExtendFootprintTTL/RestoreFootprint, the bridge call is even more wasteful (always returns 0) but these ops are far less frequent.

**Note on interaction with H001:** If H001's `getSize()` memoization is implemented, it eliminates the XDR tree walk component (~2–5µs), leaving only the FFI overhead (~0.5–1µs) as the avoidable cost here. This makes the standalone impact of H002 even smaller but still non-zero.

### PoC Guidance

- **Target code**: `src/transactions/MutableTransactionResult.cpp` — modify `RefundableFeeTracker::consumeRefundableSorobanResources` to compute the refundable fee inline instead of calling `computeSorobanResourceFee`.
- **Change description**: Replace lines 61–66 with an inline C++ computation: `int64_t eventsFee = divCeil(static_cast<int64_t>(mConsumedContractEventsSizeBytes) * sorobanConfig.feeContractEventsSize1KB(), int64_t{1024});` and assign `mConsumedRefundableFee = mConsumedRentFee + eventsFee`. Remove the `TransactionFrame const& tx` parameter since it's no longer needed for the fee computation. Update callers in `InvokeHostFunctionOpFrame.cpp`, `ExtendFootprintTTLOpFrame.cpp`, and `RestoreFootprintOpFrame.cpp` to match the new signature.
- **Correctness check**: Existing Soroban tests (`[soroban]` tag), InvokeHostFunction tests, ExtendFootprintTTL/RestoreFootprint tests, and parallel apply tests. The inline formula must exactly match `compute_fee_per_increment(contract_events_size_bytes, fee_per_contract_event_1kb, 1024)` using saturating multiplication and ceiling division. Add a protocol version gate or static_assert to catch any future protocol that changes the refundable fee formula.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` for `sac,TX=6400,T=8`. The expected improvement is <1% of total close time (Informational). Profile with Tracy to confirm the `consumeRefundableSorobanResources` ZoneScoped timing decreases.
