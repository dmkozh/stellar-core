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
