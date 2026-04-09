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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete path from `invoke_host_function_or_maybe_panic` (soroban_proto_any.rs:424) through `is_tx_tracing_enabled` (log.rs:89-93), across the CXX FFI boundary via `shim_isLogLevelAtLeast` (CppShims.h:28), into `Logging::isLogLevelAtLeast` (Logging.cpp:385-394). The C++ implementation is **significantly worse than the hypothesis anticipated**: it acquires a `std::recursive_mutex` and performs a `std::map<std::string, LogLevel>::find` on the partition name string — not a simple atomic/volatile read. With 8 parallel-apply threads, all threads contend on this same mutex per TX.

### Code Paths Examined

- `src/rust/src/soroban_proto_any.rs:391-428` — `invoke_host_function_or_maybe_panic` calls `is_tx_tracing_enabled()` at line 424, once per TX invocation, between budget construction and the actual host function call
- `src/rust/src/log.rs:89-93` — `is_tx_tracing_enabled()` creates a CXX string via `let_cxx_string!(partition = partition::TX)` and calls `shim_isLogLevelAtLeast(&partition, LogLevel::LVL_TRACE)`
- `src/rust/src/bridge.rs:386` — CXX bridge declaration for `shim_isLogLevelAtLeast` as `extern "C++"`
- `src/rust/CppShims.h:27-31` — Inline C++ shim that forwards to `Logging::isLogLevelAtLeast(partition, level)`
- `src/util/Logging.cpp:385-394` — Acquires `std::recursive_mutex mLogMutex`, performs `mPartitionLogLevels.find(partition)` (string-keyed `std::map` lookup), falls back to `mGlobalLogLevel >= level`
- `src/util/Logging.h:162-164` — Static members: `mGlobalLogLevel`, `mPartitionLogLevels` (std::map), `mLogMutex` (std::recursive_mutex)

### Findings

**The inefficiency is real and worse than hypothesized.** The hypothesis anti-evidence point 3 guessed the C++ side was "probably just an atomic or volatile read" — but it is actually:

1. **Recursive mutex acquisition** (`std::lock_guard<std::recursive_mutex>` at Logging.cpp:387) — recursive mutexes are heavier than regular mutexes due to ownership tracking
2. **String-keyed std::map lookup** (`mPartitionLogLevels.find(partition)` at Logging.cpp:388) — tree traversal with string comparisons
3. **CXX string construction + destruction** on the Rust side (let_cxx_string! macro at log.rs:90)
4. **FFI boundary crossing** (extern "C++" call through CXX bridge)

Under parallel apply with T=8, all 8 threads contend on `mLogMutex` for every single transaction. The mutex contention adds cache-line bouncing and potential serialization.

**However, the absolute impact is small relative to total benchmark time.** Per-TX cost of this call chain under 8-thread contention is estimated at 2–10μs. For a typical Soroban TX taking 0.5–5ms, this represents 0.04–2% of per-TX time. For a 100-TX ledger close, the total savings would be 200–1000μs out of a total close time of 50–500ms, which is 0.02–2% — well below the 5% threshold for Low severity.

**The fix is correct and safe.** An `AtomicBool` cache on the Rust side (e.g., `static TRACE_ENABLED: AtomicBool`) checked with `Ordering::Relaxed` would eliminate the FFI call entirely. Invalidation is trivial since log levels only change via admin commands (calls to `Logging::setLogLevel`), which could set the AtomicBool via a new FFI callback, or the cache could simply be refreshed once per ledger close.

**No existing optimizations cover this.** There is no caching, pooling, or batching of log level checks anywhere in the bridge layer.

### PoC Guidance

- **Target code**: `src/rust/src/log.rs` — add a `static TRACE_ENABLED: AtomicBool` cache; `src/rust/src/soroban_proto_any.rs:424` — use cached value; optionally add a `refresh_log_level_cache()` function called once per ledger close from the C++ side
- **Change description**: Replace per-TX FFI call with a Rust-side `AtomicBool` read. The cache can be refreshed (a) once per ledger close batch, (b) via a new bridge function called from `Logging::setLogLevel`, or (c) on a timer. Option (a) is simplest and sufficient since log levels don't change mid-ledger-close.
- **Correctness check**: Existing Soroban invocation tests (e.g., `[soroban]` tagged tests) should continue to pass since trace hooks are disabled in tests anyway. The `StellarLogger::enabled()` method (log.rs:114) should NOT be changed — it needs the per-call FFI check since it handles arbitrary partitions.
- **Benchmark focus**: Per-TX bridge overhead in apply-load benchmark. The improvement will be in the sub-microsecond range per TX (eliminating ~2-10μs of mutex+FFI overhead). Look for reduced lock contention in perf profiles under parallel apply. Total benchmark improvement is expected to be <1% — this is an Informational finding, not a benchmark-moving optimization.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-09
**PoC by**: claude-opus-4-6, high

### Changes Made

- `src/rust/src/log.rs:48-49` — Added two static `AtomicBool` variables: `TX_TRACE_ENABLED` (caches the result) and `TX_TRACE_CACHE_VALID` (tracks cache validity). Both initialized to `false`.
- `src/rust/src/log.rs:88-90` — In `init_logging()`, added cache invalidation (`TX_TRACE_CACHE_VALID.store(false, Release)`) after `set_max_level`. This is called from C++ `Logging::setLogLevel()` → `deinit()` → `init()` → `rust_bridge::init_logging()`, so the cache is automatically invalidated whenever log levels change.
- `src/rust/src/log.rs:94-107` — Rewrote `is_tx_tracing_enabled()` with a fast path that reads the cached `AtomicBool` (single `Acquire` load + `Relaxed` load), and a slow path that falls through to the original FFI call, caches the result, and marks the cache valid. Uses standard flag-guarded publication pattern with `Acquire`/`Release` ordering.

### Demonstration

The optimization replaces a per-TX FFI round-trip (CXX string construction → extern "C++" call → recursive mutex acquisition → std::map lookup → return) with a single `AtomicBool::load(Acquire)` on the fast path. Under parallel apply with T=8, this eliminates mutex contention on `Logging::mLogMutex` across all 8 threads for every transaction. The cache is invalidated whenever `Logging::setLogLevel` is called (via the existing `init_logging` bridge callback), ensuring correctness when log levels change.

### Test Results

Full test suite passes: `make check` with NUM_PARTITIONS=$(nproc) completed with exit code 0. All C++ Catch2 tests (selftest-nopg), non-determinism checks (check-nondet), and all Rust soroban-env-host tests pass with no regressions.
