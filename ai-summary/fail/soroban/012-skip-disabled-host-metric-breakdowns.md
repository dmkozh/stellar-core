# H012: Skip Disabled Host-Metric Breakdown Bookkeeping

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: Benchmark-mode CPU bookkeeping
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When `DISABLE_SOROBAN_METRICS_FOR_TESTING = true`, the invoke-host hot path
should keep only the counters needed for correctness-critical resource checks.
Telemetry-only per-entry and per-event breakdown fields should not be updated if
their publication path is disabled.

## Mechanism

`HostFunctionMetrics` checks `mDisableMetrics` only in its destructor and
`getExecTimer()`, but `noteDiskReadEntry`, `noteWriteEntry`, and `collectEvents`
still update a large set of telemetry-only fields on every entry and event. In
the apply-load benchmark config those fields are discarded: diagnostics are off,
metadata is off, and the destructor returns before publishing Medida metrics.

## Trigger

Run apply-load SAC with the stock benchmark config and profile
`HostFunctionMetrics::noteDiskReadEntry`, `noteWriteEntry`, and the event-size
loop in `collectEvents`. If this angle matters, those functions should show up
despite `DISABLE_SOROBAN_METRICS_FOR_TESTING = true`.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:14-24,49-52` — benchmark explicitly disables Soroban metrics, metadata output, and diagnostics
- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics:107-254` — `mDisableMetrics` only gates destructor publication and timer creation
- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics::noteDiskReadEntry:208-225` — updates read breakdown and max fields on every entry
- `src/transactions/InvokeHostFunctionOpFrame.cpp:HostFunctionMetrics::noteWriteEntry:227-243` — updates write breakdown and max fields on every entry
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::collectEvents:714-743` — updates event-count and max-event telemetry on every event

## Evidence

The benchmark disables the downstream consumers of these breakdown metrics, but
the hot path still performs the local integer bookkeeping. The code already
distinguishes between correctness-critical aggregates (`mLedgerReadByte`,
`mLedgerWriteByte`, `mEmitEventByte`) and telemetry-only fields, so there is a
clean conceptual fast path available.

## Anti-Evidence

These operations are only stack-local integer adds and `std::max` calls on a
small struct. They do not allocate, do not touch shared state, and run far less
often than the Soroban host itself.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The bookkeeping is too cheap to matter. Even in batched SAC, the disabled-path
work is just a few hundred cache-hot scalar operations per transaction, which is
well below 1% of Soroban invoke time and far below the benchmark noise floor.

### Lesson Learned

Benchmark-only fast paths are only worth pursuing when they remove allocations,
hashing, serialization, or synchronization. Pure scalar bookkeeping usually
looks suspicious in code review but does not move apply-load numbers.
