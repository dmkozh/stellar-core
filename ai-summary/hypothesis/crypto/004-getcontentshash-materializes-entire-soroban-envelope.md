# H004: `getContentsHash` Materializes The Full Soroban Preimage

**Date**: 2026-04-08
**Subsystem**: crypto
**Severity**: Medium
**Impact**: CPU and allocation overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Computing a transaction's contents hash should stream XDR bytes directly into SHA-256, the same way `xdrSha256` streams full-object hashing without an intermediate buffer. Signed Soroban transactions should not need to allocate and copy a complete `(networkID, envelopeType, tx)` preimage just to obtain the hash used by signature checking.

## Mechanism

`TransactionFrame::getContentsHash` calls `sha256(xdr::xdr_to_opaque(...))`, which first serializes the entire preimage into a temporary vector and then hashes that vector. Soroban envelopes in the apply-load benchmark carry large footprints, auth entries, and resource sections, so every transaction pays a full encode-to-buffer plus hash pass before any signature verification can begin.

## Trigger

Run the default `custom_token` or `soroswap` apply-load scenarios, where Soroban envelopes are larger than minimal SAC transfers. Compare allocator and memcpy samples in `TransactionFrame::getContentsHash` against a build that hashes the XDR stream directly.

## Target Code

- `src/transactions/TransactionFrame.cpp:132-154` - `getContentsHash` serializes then hashes
- `src/transactions/TransactionFrame.cpp:1904-1906` - `checkValid` constructs `SignatureChecker` from the contents hash
- `src/transactions/TransactionFrame.cpp:2066-2067` - apply path constructs `SignatureChecker` from the contents hash
- `src/transactions/TransactionFrame.cpp:2548-2549` - apply entry point always requests the contents hash
- `src/crypto/XDRHasher.h:13-104` - existing zero-copy XDR hashing infrastructure

## Evidence

The code already uses `xdrSha256(mEnvelope)` for `getFullHash`, which shows the project has an allocation-free XDR hashing pattern available. `getContentsHash` is the outlier: it materializes the whole preimage despite being executed once for every benchmark transaction.

## Anti-Evidence

`mContentsHash` is cached on the `TransactionFrame`, so repeated call sites within the same transaction do not multiply the cost. The savings therefore depend on the total number and size of distinct transactions rather than on repeated hashing within one tx.
