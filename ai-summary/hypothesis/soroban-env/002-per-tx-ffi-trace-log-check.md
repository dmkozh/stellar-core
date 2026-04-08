# H002: Per-Invocation FFI Call for Trace Log Level Check

**Date**: 2026-04-08
**Subsystem**: soroban-env (C++↔Rust bridge)
**Severity**: Low
**Impact**: 5–10% reduction in bridge overhead per TX; eliminates one FFI round-trip per Soroban invocation
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

The trace log level check for whether to enable the Soroban trace hook should
be performed at most once per ledger close (or cached with invalidation when
log levels change), not on every individual transaction invocation. The log
level is a global configuration that changes extremely rarely during operation.

## Mechanism

Every call to `invoke_host_function_or_maybe_panic` (soroban_proto_any.rs:424)
calls `crate::log::is_tx_tracing_enabled()` which:

1. Creates a CXX string for the partition name "Tx" (allocation).
2. Calls `shim_isLogLevelAtLeast(&partition, LogLevel::LVL_TRACE)` which
   crosses the Rust→C++ FFI boundary.
3. The C++ shim calls `Logging::isLogLevelAtLeast(partition, level)`.
4. Result propagates back across FFI.

This FFI round-trip includes: CXX string construction, function pointer
indirect call through the extern "C" thunk, error handling wrapper, and
CXX string destruction. While each call is individually cheap (~0.5-2μs),
it adds up across hundreds of transactions per ledger close.

For parallel apply with T=8, all 8 threads independently make this FFI call
per transaction, even though they would all get the same answer.

## Trigger

Run the apply-load benchmark and profile the `is_tx_tracing_enabled` function.
It will show up in every transaction's call stack, called once per
`invoke_host_function_or_maybe_panic`.

## Target Code

- `src/rust/src/soroban_proto_any.rs:423-428` — Per-invocation check: `if crate::log::is_tx_tracing_enabled()`
- `src/rust/src/log.rs:89-93` — `is_tx_tracing_enabled` implementation crossing FFI
- `src/rust/CppShims.h:shim_isLogLevelAtLeast` — C++ shim function

## Evidence

1. `is_tx_tracing_enabled()` (log.rs:89-93) explicitly calls
   `shim_isLogLevelAtLeast` which is an `extern "C++"` bridge function,
   confirmed by the CXX bridge declaration in bridge.rs:386.

2. The function is called unconditionally on every invocation at
   soroban_proto_any.rs:424, inside the hot path between budget creation
   and the actual host function call.

3. The `let_cxx_string!` macro at log.rs:90 creates a `Pin<&mut CxxString>`
   on every call, which involves stack allocation and initialization of the
   partition name string.

4. In production and benchmarking, trace-level logging is almost never enabled,
   so this call almost always returns false — making the entire FFI round-trip
   pure waste in the common case.

## Anti-Evidence

1. Individual FFI call overhead is small (~0.5-2μs). The total impact across
   a ledger close with 100 TXs is ~50-200μs, which may be negligible compared
   to total ledger close time.

2. An `AtomicBool` cache would need invalidation when log levels change. The
   current call-through approach is simpler and always correct.

3. The `shim_isLogLevelAtLeast` call in C++ is likely very fast (probably just
   an atomic or volatile read of a log level value), so the dominant cost is
   the FFI crossing overhead itself, not the actual check.
