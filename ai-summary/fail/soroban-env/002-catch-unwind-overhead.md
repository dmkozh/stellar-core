# H002-FAIL: panic::catch_unwind Overhead Per Invocation

**Date**: 2026-04-08
**Subsystem**: soroban-env (C++↔Rust bridge)
**Severity**: Informational
**Impact**: N/A
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

The `panic::catch_unwind` wrapper around each host function invocation should
add minimal overhead, but if it involves significant setup (landing pads,
personality functions), it could be a per-TX cost worth eliminating.

## Mechanism

Every `invoke_host_function` call (soroban_proto_any.rs:325) wraps the actual
work in `panic::catch_unwind(panic::AssertUnwindSafe(|| ...))`. This is a
safety measure to prevent Rust panics from unwinding across the FFI boundary
into C++.

## Trigger

Profile `panic::catch_unwind` overhead during apply-load benchmark.

## Target Code

- `src/rust/src/soroban_proto_any.rs:invoke_host_function:325-354` — catch_unwind wrapper

## Evidence

`catch_unwind` is called on every single host function invocation. The
`AssertUnwindSafe` wrapper also captures the closure environment.

## Anti-Evidence

On modern x86-64 architectures with table-based (DWARF) exception/unwind
handling, `catch_unwind` has zero runtime cost on the non-panic path. The
tables are only consulted when an actual unwind occurs. The compiler generates
no extra instructions for the "happy path" — only metadata in `.eh_frame`
sections that are not loaded into cache during normal execution.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

`panic::catch_unwind` uses zero-cost table-based unwinding on x86-64 Linux
(the benchmark target). On the non-panic path (which is every normal
invocation), the function compiles to essentially no additional instructions
beyond the function call itself. The unwind tables are stored in read-only
`.eh_frame` sections that are not accessed during normal execution and do not
pollute the instruction or data caches.

Additionally, `AssertUnwindSafe` is a zero-sized wrapper that exists only for
the type system — it compiles away entirely.

The safety benefit (preventing UB from unwinding across FFI) far outweighs
the zero measurable cost.

### Lesson Learned

On x86-64 with DWARF unwinding, `catch_unwind` and try/catch are zero-cost
abstractions on the non-exception path. Do not target these as optimization
opportunities. Focus instead on work that produces actual instructions
(serialization, allocation, computation).
