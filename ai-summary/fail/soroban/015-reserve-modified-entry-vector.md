# H015: Pre-Reserve `modified_entries` in `extract_ledger_effects`

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Informational
**Impact**: CPU / tiny allocator churn in Rust bridge result assembly
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The Rust bridge should avoid avoidable vector reallocations while assembling the
`modified_ledger_entries` result. If the number of returned entries is already
bounded by `entry_changes.len()`, the output vector should ideally reserve that
capacity up front.

## Mechanism

`extract_ledger_effects` starts with `let mut modified_entries = vec![];` and
pushes encoded new values plus synthetic TTL entries into it. Reserving capacity
from `entry_changes.len()` would remove a handful of `Vec<RustBuf>` growth
steps and small-struct moves while assembling the bridge result.

## Trigger

Profile `src/rust/src/soroban_proto_any.rs:extract_ledger_effects` in a
batched-SAC benchmark where a single invocation can emit on the order of a few
hundred modified entries.

## Target Code

- `src/rust/src/soroban_proto_any.rs:extract_ledger_effects:261-301` — appends to `modified_entries` without reserving
- `src/rust/soroban/p25/soroban-env-host/src/e2e_invoke.rs:get_ledger_changes:183-199` — upstream diff builder already knows a bound and reserves its own output vector

## Evidence

The bridge output vector starts empty even though `entry_changes.len()` is an
obvious upper bound on the number of encoded new-value pushes, and a close
approximation for the final count after TTL additions. This is a real, local
allocation inefficiency.

## Anti-Evidence

Each reallocation only moves `RustBuf` headers, not the underlying encoded byte
buffers, and the per-transaction element count is still modest. Even in batched
SAC, the total avoided work is a few small `Vec` growth steps for O(10^2)
elements, which is far below the threshold needed to move apply-load results.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The inefficiency is real but much too small: reserving this vector only removes
reallocation of small wrapper structs while all encoded payload allocation and
host execution costs remain unchanged. The expected savings are well below 1% of
any apply-load scenario.

### Lesson Learned

In this bridge, vector-growth cleanups are only interesting when the vector owns
large payload copies or the element count is enormous. When elements are just
wrappers around already-allocated byte buffers, reserve-only changes are rarely
benchmark-relevant.
