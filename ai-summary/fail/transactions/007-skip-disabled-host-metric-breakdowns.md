# H002: Metrics-disabled apply still maintains per-entry breakdown counters that no benchmark consumer reads

**Date**: 2026-04-10
**Subsystem**: transactions
**Severity**: Low
**Impact**: per-entry/event bookkeeping in the invoke-host hot loop
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When `DISABLE_SOROBAN_METRICS_FOR_TESTING = true`, the apply-load path should retain only the minimal counters needed for correctness checks such as read-byte, write-byte, and emitted-event-byte enforcement. It should not keep updating telemetry-only breakdowns like read/write key bytes, code/data byte splits, max entry sizes, or event-count histograms that will never be published in the benchmark configuration.

## Mechanism

`HostFunctionMetrics` only checks `mDisableMetrics` in its destructor and timer helper, but the hot read/write/event paths still update many metrics-only fields on every entry and event. In the benchmark configs those fields are dead: resource-limit enforcement only consumes aggregates like `mLedgerReadByte`, `mLedgerWriteByte`, and `mEmitEventByte`, while the destructor returns before publishing any of the fine-grained counters.

## Trigger

Run `scripts/run_apply_load_matrix.py` with the stock benchmark configs (`docs/apply-load-benchmark-sac.cfg` or `docs/apply-load-benchmark-token.cfg`). Profile `HostFunctionMetrics::noteDiskReadEntry`, `noteWriteEntry`, and `collectEvents` on `sac,TX=6400,T=8`; expect repeated `std::max` and counter updates for fields that are discarded because metrics are disabled.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:14-18` — benchmark explicitly disables Soroban metrics
- `docs/apply-load-benchmark-token.cfg:14-18` — same for custom-token benchmark
- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics:106-206` — `mDisableMetrics` only gates destructor publication
- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics::noteDiskReadEntry:208-225` — updates breakdown/max counters on every read
- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics::noteWriteEntry:227-243` — updates breakdown/max counters on every write
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::collectEvents:713-753` — updates telemetry-only event counters even when metrics are disabled

## Evidence

The benchmark configs explicitly document that Medida metrics in the apply path are disabled to avoid benchmark distortion. Despite that, the invoke-host helper still performs per-entry updates to `mReadKeyByte`, `mReadDataByte`, `mReadCodeByte`, `mWriteDataByte`, `mWriteCodeByte`, `mMaxReadWrite*`, `mEmitEvent`, and `mMaxEmitEventByte`; none of those fields participate in budget enforcement, and the destructor drops them on the floor when `mDisableMetrics` is true.

## Anti-Evidence

Some fields inside `HostFunctionMetrics` are correctness-critical even with metrics disabled: the code still needs `mLedgerReadByte`, `mLedgerWriteByte`, and `mEmitEventByte` to enforce Soroban limits, and `mSuccess` is still used for optional diagnostic-event emission when diagnostics are enabled. Any fast path therefore has to split telemetry-only bookkeeping from resource-accounting state rather than bypassing the struct wholesale.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced `HostFunctionMetrics` struct (lines 107-254), its `noteDiskReadEntry` (lines 208-225), `noteWriteEntry` (lines 227-243), and the `collectEvents` method (lines 706-754). Confirmed the hypothesis is technically correct: telemetry-only fields (`mReadKeyByte`, `mReadDataByte`, `mReadCodeByte`, `mWriteDataByte`, `mWriteCodeByte`, `mWriteEntry`, `mReadEntry`, `mMaxReadWriteKeyByte`, `mMaxReadWriteDataByte`, `mMaxReadWriteCodeByte`, `mEmitEvent`, `mMaxEmitEventByte`) are updated on every entry/event but discarded in the destructor when `mDisableMetrics` is true. However, the per-invocation cost of these operations is negligible — they are simple integer additions and `cmov` instructions on L1-cached struct members.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics:107-254` — struct is ~128 bytes, fits in 2 cache lines. All fields are stack-local (struct is a member of `InvokeHostFunctionApplyHelper`). `mDisableMetrics` gates only the destructor (line 153) and `getExecTimer()` (line 248).
- `src/transactions/InvokeHostFunctionOpFrame.cpp:noteDiskReadEntry:208-225` — 5 telemetry-only operations per call: `mReadEntry++`, `mReadKeyByte += keySize`, `mMaxReadWriteKeyByte = std::max(...)`, plus one of `mReadCodeByte/mReadDataByte += entrySize` and `mMaxReadWriteCodeByte/mMaxReadWriteDataByte = std::max(...)`. The `mLedgerReadByte += entrySize` update is correctness-critical (consumed by `meterDiskReadResource` at line 338).
- `src/transactions/InvokeHostFunctionOpFrame.cpp:noteWriteEntry:227-243` — 5 telemetry-only operations per call: `mWriteEntry++`, `mMaxReadWriteKeyByte = std::max(...)`, plus one of `mWriteCodeByte/mWriteDataByte += entrySize` and corresponding max. The `mLedgerWriteByte += entrySize` update is correctness-critical (consumed at line 641).
- `src/transactions/InvokeHostFunctionOpFrame.cpp:collectEvents:706-754` — 2 telemetry-only operations per event: `mEmitEvent++` and `mMaxEmitEventByte = std::max(...)`. The `mEmitEventByte += eventSize` update is correctness-critical (consumed at lines 721-722, 741-742, and passed to `consumeRefundableSorobanResources` at line 760).
- `src/transactions/InvokeHostFunctionOpFrame.cpp:maybePopulateMetricsInDiagnosticEvents:831-881` — reads all telemetry fields but has early return (line 835-838) when `!cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS`, which is the benchmark configuration.

### Why It Failed

The inefficiency is technically real but the operations are trivially cheap — simple integer additions (`add`) and conditional moves (`cmov`) on struct members that are already in L1 cache. Per transaction, `noteDiskReadEntry` is called ~10-15 times (footprint size) and `noteWriteEntry` ~3-5 times (modified entries), yielding ~100 telemetry-only integer operations per transaction. At 1 cycle each on a modern CPU, this totals ~30-50 nanoseconds per transaction. Across a full 6400-tx benchmark, the total savings would be ~0.2-0.3ms — deep in the noise floor of multi-second benchmark runs. No branch misprediction is involved (the code/data branch is predictable; `std::max` compiles to branchless `cmov`), and no cache misses occur since the struct is contiguous and hot.

For comparison, the same code path performs `xdr::xdr_to_opaque` serialization (heap-allocating `toCxxBuf` per entry), FFI calls into the Rust host, and XDR deserialization of results — each of which costs 100-1000x more than the integer counter updates being targeted.

### Lesson Learned

Integer counter updates on L1-cached, stack-local structs cost ~1 CPU cycle each and are effectively free compared to the real costs on the same code path (XDR serialization, heap allocation, FFI). When evaluating per-entry overhead in invoke-host, focus on operations that touch memory (allocations, copies, serializations) or cross boundaries (FFI, syscalls), not on bookkeeping arithmetic.
