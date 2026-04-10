# H001: Cache `is_tx_tracing_enabled()` to Avoid Per-TX Rust→C++ FFI Round-Trip with Global Mutex

**Date**: 2025-07-18
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / FFI overhead / mutex contention under parallel apply
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When the log level is below TRACE (which is the case in all production and
benchmark scenarios), the trace-hook check in `invoke_host_function_or_maybe_panic`
should short-circuit immediately without crossing the FFI boundary. The cost
should be ~3-5 ns (a single atomic read of the Rust `log::max_level()` static),
not ~140-290 ns per TX.

## Mechanism

Every Soroban TX invocation calls `crate::log::is_tx_tracing_enabled()`
(soroban_proto_any.rs:434) to decide whether to install a trace hook. This
function:

1. Allocates a `CxxString` on the Rust stack via `let_cxx_string!(partition = "Tx")`
2. Crosses Rust→C++ FFI to call `shim_isLogLevelAtLeast`
3. The C++ side acquires `std::recursive_mutex mLogMutex` (Logging.cpp:387) —
   a **global static mutex** shared across ALL threads
4. Performs `std::map<std::string, LogLevel>::find("Tx")` on the partition map
5. Returns the result back across FFI

Unlike `debug!`/`trace!` macros in the Rust `log` crate (which check
`log::max_level()` first and short-circuit when the level is below the
threshold), `is_tx_tracing_enabled()` bypasses the Rust-side level filter and
always makes the full FFI + mutex round-trip. In benchmarks and production
where global log level is INFO, `max_level()` is `LevelFilter::Info`, which
would immediately return `false` for a TRACE check — but this short-circuit
is missing.

Under T=8 parallel apply, all 8 worker threads contend on the same
`mLogMutex` recursive_mutex on every TX, adding cache-line bouncing overhead
(~50-100 ns per cross-core cache line transfer) on top of the base lock cost.

## Trigger

Run any apply-load benchmark scenario (SAC, custom_token, soroswap) at T=1
or T=8. The overhead is proportional to TX count:
- SAC @ 3200 TXs: ~450-930 μs per ledger
- T=1 baseline ~2500 ms: ~0.02-0.04%
- T=8 baseline ~350 ms: ~0.13-0.27% (plus cache-line bouncing overhead)

## Target Code

- `src/rust/src/log.rs:89-93` — `is_tx_tracing_enabled()` calls `shim_isLogLevelAtLeast` without checking `log::max_level()` first
- `src/rust/src/soroban_proto_any.rs:433-438` — per-TX call site in `invoke_host_function_or_maybe_panic`
- `src/rust/CppShims.h:28-31` — C++ shim delegates to `Logging::isLogLevelAtLeast`
- `src/util/Logging.cpp:385-394` — `isLogLevelAtLeast` acquires `mLogMutex` (recursive_mutex) and does `std::map::find`
- `src/util/Logging.h:162-164` — `mGlobalLogLevel`, `mPartitionLogLevels`, `mLogMutex` are all static (global)

## Evidence

1. **Missing short-circuit**: `is_tx_tracing_enabled()` goes directly to FFI
   without checking `log::max_level()`, unlike all `debug!`/`trace!` macro
   uses which benefit from the `log` crate's built-in level filter. The Rust
   `init_logging` (log.rs:68-87) sets `log::set_max_level(maxFilter)` based
   on the C++ global log level, so the short-circuit value is always correct.

2. **Global mutex in hot path**: `mLogMutex` is a static `std::recursive_mutex`
   (Logging.h:164) that protects all log level queries. Under T=8, 8 threads ×
   400 TXs/thread all acquire this mutex, causing the mutex cache line to
   bounce between cores.

3. **Consistent overhead**: Unlike some bridge costs that only apply to cache
   misses or specific entry types, this cost is paid on EVERY successful TX
   invocation unconditionally.

## Anti-Evidence

1. **Individual cost is very small**: ~140-290 ns per TX is well below the
   1 μs/TX noise threshold identified in the meta-patterns.

2. **Contention probability is low**: With ~100 ns lock hold time and ~875 μs
   per-TX processing time at T=8, actual lock contention probability is <0.1%
   per attempt. The cache-line bouncing cost (not contention per se) is the
   larger concern but adds only ~50-100 ns.

3. **Fix is trivial**: Adding `if log::max_level() < log::LevelFilter::Trace { return false; }`
   as the first line of `is_tx_tracing_enabled()` would eliminate the FFI
   overhead entirely. The fix is so small it might not justify a formal PoC.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of fail/soroban-env/004 (condensed in summary.md)
**Failed At**: reviewer

### Trace Summary

This hypothesis is a direct duplicate of previously investigated fail entry 004. That investigation implemented an `AtomicBool` cache for `is_tx_tracing_enabled()`, which passed the full test suite but regressed 5/6 benchmark scenarios (SAC T=8: −18–22%) when independently benchmarked via `run_apply_load_matrix.py`. The regression was attributed to benchmark noise, confirming the optimization is too small to produce a measurable improvement.

### Code Paths Examined

- `src/rust/src/log.rs:89-93` — `is_tx_tracing_enabled()` confirmed to call FFI without Rust-side short-circuit
- `src/rust/src/soroban_proto_any.rs:433-438` — per-TX call site confirmed

### Why It Failed

This is a duplicate of fail/soroban-env/004, which was fully investigated through PoC and final-review stages. The PoC (AtomicBool cache) was correct but produced no measurable benchmark improvement — the ~140-290 ns/TX savings falls below the benchmark noise floor, consistent with meta-pattern #1: "Small optimizations that save <1 µs/TX typically cannot overcome measurement noise."

### Lesson Learned

The fail summary should be checked before generating hypotheses targeting `is_tx_tracing_enabled()`. This specific optimization has been thoroughly investigated and cannot produce measurable improvements despite being theoretically correct.
