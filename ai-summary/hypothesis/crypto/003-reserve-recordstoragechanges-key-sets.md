# H003: Reserve `recordStorageChanges()` Key Sets Before Decoding Large Host Outputs

**Date**: 2026-04-10
**Subsystem**: crypto, transactions
**Severity**: Medium
**Impact**: post-host rehashing of expensive `LedgerKey`s during storage-change reconciliation
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once Rust has returned the number of modified ledger entries for a host
invocation, the C++ post-processing path should pre-size its temporary key sets
from that count before inserting decoded keys. Large SAC outputs should not
force `recordStorageChanges()` to grow and rehash `createdAndModifiedKeys` and
`createdKeys` while it is already paying to decode every entry.

## Mechanism

`recordStorageChanges()` creates two empty `UnorderedSet<LedgerKey>` instances
and then inserts every returned key into `createdAndModifiedKeys`, and inserts
again into `createdKeys` when `upsertLedgerEntry()` reports a logical create.
The batched SAC benchmark generates fresh destination contract addresses per tx,
so each 100-destination transfer tends to create around 100 new balance entries
plus their TTL entries, producing a large `createdKeys` set in addition to the
full modified-entry set. Growing both sets from zero triggers repeated rehashes
of `CONTRACT_DATA` balance keys exactly where the hot path has just decoded them
from host output.

## Trigger

Run SAC apply-load with batched transfers and profile `recordStorageChanges()`,
`unordered_set` growth, and `std::hash<LedgerKey>` after host invocation.
Compare against a build that reserves `createdAndModifiedKeys` from
`out.modified_ledger_entries.size()` and reserves `createdKeys` from an estimate
based on the write footprint / modified-entry count before the decode loop.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:31-38` — SAC benchmark uses a 100-destination batch size
- `src/simulation/ApplyLoad.cpp:2094-2103` — batched SAC destinations are intentionally unique per tx
- `src/simulation/TxGenerator.cpp:1492-1512` — each unique destination becomes a distinct `Balance` `CONTRACT_DATA` key in the write footprint
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-703` — `recordStorageChanges()` builds two empty key sets, inserts decoded keys, then probes them again during deletion inference
- `src/transactions/InvokeHostFunctionOpFrame.cpp:655-658` — `createdKeys` grows on every logical create
- `src/ledger/LedgerHashUtils.h:178-184` — hashing each `Balance` contract-data key requires `shortHash::xdrComputeHash` over the SCVal key

## Evidence

The benchmarked SAC path deliberately creates fresh destination addresses, so
the post-host reconciliation loop does not just update existing keys — it often
creates many new `Balance` and TTL entries in the same tx. Both temporary sets
start at zero capacity even though the code already knows the exact modified
entry count before the first insertion. That combination makes repeated
rehashing avoidable on a very regular benchmark workload.

## Anti-Evidence

`custom_token` transfers update existing balances and have much smaller outputs,
so `createdKeys` is not large there. This hypothesis is therefore aimed at the
batched SAC scenario first; the other models may show only marginal gains.
