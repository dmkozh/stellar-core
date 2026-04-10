# H009: Duplicate `SorobanNetworkConfig::loadFromLedger` calls in finalize path

**Date**: 2026-04-10
**Subsystem**: storage (ledger)
**Severity**: Informational
**Impact**: per-ledger config-read overhead during commit
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The commit path should avoid reloading the full Soroban network configuration
multiple times from the same `LedgerTxn` if no intervening mutation can change
the effective configuration. Reusing an already loaded config object would be
preferable when semantically valid.

## Mechanism

`finalizeLedgerTxnChanges(...)` loads `SorobanNetworkConfig` once before
eviction-resolution work and then loads it again after `maybeSnapshotSorobanStateSize(...)`.
At first glance this looks like two full scans over the same configuration
entries inside a single ledger-close commit.

## Trigger

Run any Soroban ledger close. The finalize path will execute both config loads
whenever the ledger version is at or above the Soroban protocol.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2969-2972` — first `SorobanNetworkConfig::loadFromLedger(ltx)`
- `src/ledger/NetworkConfig.cpp:2347-2386` — `maybeSnapshotSorobanStateSize(...)` mutates the state-size window setting inside `ltx`
- `src/ledger/LedgerManagerImpl.cpp:3044-3047` — second config load used for the final post-commit snapshot

## Evidence

The two explicit `loadFromLedger(...)` calls are visible in the finalize path
and both run on Soroban ledgers. Reloading a complex config object twice in one
serial commit phase is the kind of redundancy worth checking.

## Anti-Evidence

The second load is not redundant after all: `maybeSnapshotSorobanStateSize(...)`
updates the on-ledger state-size sampling window in between the two calls, so
the final config object must observe that mutation. The configuration surface is
also tiny relative to bucket serialization, hashing, and fsync work, so even a
single avoided reload would only save a very small amount of CPU.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

`maybeSnapshotSorobanStateSize(...)` mutates the config-setting ledger entry
between the two loads, so the second `loadFromLedger(...)` is semantically
required to capture the updated rolling state-size window. The remaining cost is
too small to matter compared with the bucket and write-path work in the same
commit phase.

### Lesson Learned

Repeated reads of the same subsystem object are only worth optimizing if no
intervening write can legally change the object. In the ledger finalize path,
state-size snapshotting itself mutates config state, so a second config load is
not automatically redundant.
