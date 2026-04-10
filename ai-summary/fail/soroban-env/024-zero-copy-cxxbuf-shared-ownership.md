# H024: Zero-Copy CxxBuf via SharedPtr or Pointer-as-Integer Encoding

**Date**: 2026-04-10
**Subsystem**: soroban-env (bridge layer)
**Severity**: Low
**Impact**: Eliminate per-TX memcpy for cached RO entries without bridge API restructuring
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

Cached RO entry bytes should be passable to the Rust bridge without copying, by
using shared ownership (`SharedPtr<CxxVector<u8>>`) or raw pointer encoding
(`u64` pointer + `u64` length) in the `CxxBuf` type. This would eliminate the
per-TX `make_unique<vector<uint8_t>>(cached_bytes)` copy for large entries.

## Mechanism

The `CxxBuf` struct uses `UniquePtr<CxxVector<u8>>` which enforces unique
ownership. Changing to `SharedPtr<CxxVector<u8>>` would allow the RO
serialization cache and the CxxBuf to share the same allocation. Alternatively,
encoding a raw pointer as `u64` in a `CxxBufView` struct would provide zero-copy
access.

## Trigger

Same as H001 — large RO entries (46 KB token.wasm, 118 KB soroswap total) are
copied per TX from the serialization cache. Zero-copy would eliminate ~3–14 μs
per TX. Over 1600–3000 TXs: 5–42 ms savings.

## Target Code

- `src/rust/src/bridge.rs:13-15` — `CxxBuf` definition with `UniquePtr<CxxVector<u8>>`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1206-1207` — Copy constructor call

## Evidence

- The per-TX copy cost is real (~3–5 μs per 46 KB entry)
- SharedPtr would eliminate the copy while maintaining memory safety

## Anti-Evidence

- CXX does not support `SharedPtr<CxxVector<u8>>` — `CxxVector<T>` is a mapped type, not an opaque C++ type, so it cannot be wrapped in SharedPtr within the CXX bridge
- The pointer-as-u64 approach requires `unsafe` Rust code in a correctness-critical path

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (fail #008 analyzed caching but not shared ownership)

### Why It Failed

CXX's type system does not support `SharedPtr<CxxVector<T>>` because
`CxxVector<T>` is a built-in mapped type, not a user-defined opaque C++ type.
The `SharedPtr` wrapper in CXX only works with opaque types declared in the
`extern "C++"` block. Wrapping the vector in a custom opaque type (e.g.,
`SharedByteBuffer`) would require substantial bridge redesign, negating the
simplicity advantage over the separate RO/RW vector approach (H001).

The pointer-as-u64 alternative introduces `unsafe` Rust code
(`std::slice::from_raw_parts`) in the ledger apply hot path. The safety
invariant (C++ must keep the pointer valid until the Rust call returns) is
maintainable but adds a correctness risk disproportionate to the ~2–3% savings.

### Lesson Learned

CXX's `SharedPtr<T>` only wraps opaque C++ types, not mapped types like
`CxxVector<T>` or `CxxString`. Any zero-copy approach for bridge data must
work within CXX's type constraints, which means either (a) using opaque types
with accessor methods, or (b) restructuring the API to avoid copies at a
higher level (as H001 proposes).
