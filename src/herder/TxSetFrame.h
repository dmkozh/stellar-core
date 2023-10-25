#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "util/NonCopyable.h"
#include "xdr/Stellar-internal.h"

#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>

namespace stellar
{
class Application;
class TxSetFrame;
class ResolvedTxSetFrame;
using TxSetFrameConstPtr = std::shared_ptr<TxSetFrame const>;

// `TxSetFrame` is a wrapper around `TransactionSet` or
// `GeneralizedTransactionSet` XDR.
//
// `TxSetFrame` generally doesn't try to interpret the XDR it wraps
// and might even store structurally invalid XDR and thus its safe to use at
// overlay layer to simply exchange the messages, cache it etc.
// In order to interpret (or 'resolve') the `TxSetFrame` use `resolve`
// function. This function is only valid when `TxSetFrame` corresponds to
// the correct ledger state, i.e. it points at the LCL hash.
//
// Here are the states of the `TxSetFrame` lifecycle:
//
// 1.  Non-resolved - wraps an arbitrary TxSetFrame XDR. Can be created at
//     any time.
// 2.  Resolved - the XDR has been interpreted using the valid ledger state.
//     Can only transition from (1) when `previousLedgerHash` equals to
//     the current LCL's hash.
// 2b. Resolved, invalid XDR - the XDR is structurally invalid and thus
//     can't be properly interpreted. Can't transition to any other states
//     from this.
// 3.  Resolved, fully validated - `checkValid()` == true for the resolved
//     frame. Can only transition from 2 when `previousLedgerHash` equals to
//     the current LCL's hash. Can be nominated/voted for/applied.
// 3b. Resolved, not valid - `checkValid()` == false for the resolved frame.
//     Can't be nominated/voted for/applied.
//
// 1->2/3 transition is not reversible, i.e. after `TxSetFrame` has been
// resolved at the correct LCL, its resolved-only functionality can be used
// at any time (e.g. for accessing the view methods).
class TxSetFrame : public NonMovableOrCopyable,
                   public std::enable_shared_from_this<TxSetFrame>
{
  public:
    enum Phase
    {
        CLASSIC,
        SOROBAN,
        PHASE_COUNT
    };

    using Transactions = std::vector<TransactionFrameBasePtr>;
    using TxPhases = std::vector<Transactions>;

    static std::string getPhaseName(Phase phase);

    // Creates a valid TxSetFrame from the provided transactions.
    //
    // The returned TxSetFrame is resolved thus the `resolve` calls on it
    // just return the already cached value.
    //
    // Not all the transactions will be included in the result: invalid
    // transactions are trimmed and optionally returned via `invalidTxs` and if
    // there are too many remaining transactions surge pricing is applied.
    // The result is guaranteed to pass `checkValid` check with the same
    // arguments as in this method, so additional validation is not needed.
    //
    // **Note**: the output `TxSetFrame` will *not* contain the input
    // transaction pointers.
    static TxSetFrameConstPtr makeFromTransactions(
        TxPhases const& txPhases, Application& app,
        uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset
#ifdef BUILD_TESTS
        // Skips the tx set validation and preserves the pointers to the
        // passed-in transactions - use in conjunction with `orderOverride`
        // argument in test-only overrides.
        ,
        bool skipValidation = false
#endif
    );
    static TxSetFrameConstPtr makeFromTransactions(
        TxPhases const& txPhases, Application& app,
        uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
        TxPhases& invalidTxsPerPhase
#ifdef BUILD_TESTS
        // Skips the tx set validation and preserves the pointers to the
        // passed-in transactions - use in conjunction with `orderOverride`
        // argument in test-only overrides.
        ,
        bool skipValidation = false
#endif
    );

    // Creates a valid empty TxSetFrame pointing at provided `lclHeader`.
    // The output TxSetFrame is not resolved as `lclHeader` doesn't necessarily
    // match the real LCL.
    static TxSetFrameConstPtr
    makeEmpty(LedgerHeaderHistoryEntry const& lclHeader);

    // `makeFromWire` methods create a TxSetFrame from the XDR messages.
    // These methods don't perform any validation on the XDR. They have to
    // be `resolve`d first, then validated with `checkValid`.
    static TxSetFrameConstPtr makeFromWire(TransactionSet const& xdrTxSet);
    static TxSetFrameConstPtr
    makeFromWire(GeneralizedTransactionSet const& xdrTxSet);

    static TxSetFrameConstPtr
    makeFromStoredTxSet(StoredTransactionSet const& storedSet);

    // Creates a legacy (non-generalized) non-resolved TxSetFrame from the
    // transactions that are trusted to be valid. Validation and filtering
    // are not performed.
    // This should be *only* used for building the legacy TxSetFrames from
    // historical transactions.
    static TxSetFrameConstPtr
    makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                TxSetFrame::Transactions const& txs);

    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;
    void storeXDR(StoredTransactionSet& txSet) const;

    ~TxSetFrame() = default;

    // Resolves this transaction set using the current ledger set.
    // The resolution includes any logic dependent on the protocol version
    // and network configuration state, i.e. any logic that depends on
    // the current ledger state. That's why this may *only* be called
    // when LCL hash matches the `previousLedgerHash` of this TxSetFrame.
    //
    // The returned value has the same lifetime as this `TxSetFrame` and
    // is cached, so it's safe and cheap to call this multiple times.
    ResolvedTxSetFrame const* resolve(Application& app,
                                      AbstractLedgerTxn& ltx) const;

    // Gets the resolved tx set frame corresponding to this tx set, if any.
    // If this tx set is not resolved yet, returns `nullptr`.
    // Prefer using `resolve` directly to get the resolved tx set frame.
    ResolvedTxSetFrame const* getResolvedFrame() const;

    bool isGeneralizedTxSet() const;

    // Returns the hash of this tx set.
    Hash const& getContentsHash() const;

    Hash const& previousLedgerHash() const;

    size_t sizeTxTotal() const;

    size_t sizeOpTotal() const;

    // Returns the size of this transaction set when encoded to XDR.
    size_t encodedSize() const;

    // Creates transaction frames for all the transactions in the set, grouped
    // by phase.
    // This is only necessary to serve a very specific use case of updating
    // the transaction queue with non-resolved tx sets. Otherwise, use
    // resolve()->getTransactionsForPhase().
    TxPhases createTransactionFrames(Hash const& networkID) const;

#ifdef BUILD_TESTS
    static TxSetFrameConstPtr makeFromTransactions(
        Transactions txs, Application& app, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset, bool enforceTxsApplyOrder = false);
    static TxSetFrameConstPtr makeFromTransactions(
        Transactions txs, Application& app, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset, Transactions& invalidTxs,
        bool enforceTxsApplyOrder = false);
#endif

  private:
    TxSetFrame(TransactionSet const& xdrTxSet);
    TxSetFrame(GeneralizedTransactionSet const& xdrTxSet);

    std::variant<TransactionSet, GeneralizedTransactionSet> mXDRTxSet;
    size_t mEncodedSize{};
    Hash mHash;

    mutable std::optional<std::unique_ptr<ResolvedTxSetFrame>> mResolvedTxSet{};
};

// Interface for the functionality only available for resolved
// `TransactionSetFrame`.
//
// This can't exist without the corresponding 'parent' `TxSetFrame`.
//
// Note: `ResolvedTxSetFrame` is intentionally not a subclass of
// `TxSetFrame`, as we'd need to convert a shared_ptr to
// as non-resolved `TxSetFrame const` to `ResolvedTxSetFrame const`,
// which doesn't work well with passing `TxSetFrameConstPtr` by value,
// which the code base does a lot.
class ResolvedTxSetFrame
{
  public:
    // Returns the base fee for the transaction or std::nullopt when the
    // transaction is not discounted.
    std::optional<int64_t> getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                        LedgerHeader const& lclHeader) const;

    Hash const& getContentsHash() const;

    // Gets all the transactions belonging to this frame in arbitrary order.
    TxSetFrame::Transactions const&
    getTxsForPhase(TxSetFrame::Phase phase) const;

    // Build a list of transaction ready to be applied to the last closed
    // ledger, based on the transaction set.
    //
    // The order satisfies:
    // * transactions for an account are sorted by sequence number (ascending)
    // * the order between accounts is randomized
    TxSetFrame::Transactions getTxsInApplyOrder() const;

    // Checks if this tx set frame is valid in the context of the current LCL.
    // This can be called when LCL does not match `previousLedgerHash`, but
    // then validation will never pass.
    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) const;

    size_t size(LedgerHeader const& lh,
                std::optional<TxSetFrame::Phase> phase = std::nullopt) const;

    size_t
    sizeTx(TxSetFrame::Phase phase) const
    {
        return mTxPhases.at(phase).size();
    }
    size_t sizeTxTotal() const;

    bool
    empty() const
    {
        return sizeTxTotal() == 0;
    }

    size_t
    numPhases() const
    {
        return mTxPhases.size();
    }

    size_t sizeOp(TxSetFrame::Phase phase) const;
    size_t sizeOpTotal() const;

    // Returns the size of this transaction set when encoded to XDR.
    size_t encodedSize() const;

    // Returns the sum of all fees that this transaction set would take.
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all _inclusion fee_ bids for all transactions in this
    // set.
    int64_t getTotalInclusionFees() const;

    // Returns whether this transaction set is generalized, i.e. representable
    // by GeneralizedTransactionSet XDR.
    bool isGeneralizedTxSet() const;

    // Returns a short description of this transaction set.
    std::string summary() const;

  private:
    friend class TxSetFrame;

    ResolvedTxSetFrame(LedgerHeaderHistoryEntry const& lclHeader,
                       TxSetFrame::TxPhases const& txs);
    ResolvedTxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
                       TxSetFrame::TxPhases const& txs);
    ResolvedTxSetFrame(ResolvedTxSetFrame const& other) = default;
    ResolvedTxSetFrame(ResolvedTxSetFrame&& other) = default;
    // Computes the fees for transactions in this set based on information from
    // the non-generalized tx set.
    // This has to be `const` in combination with `mutable` fee-related fields
    // in order to accommodate one specific case: legacy (non-generalized) tx
    // sets received from the peers don't include the fee information and we
    // don't have immediate access to the ledger header needed to compute them.
    // Hence we lazily compute the fees in `getTxBaseFee` for such TxSetFrames.
    // This can be cleaned up after the protocol migration as non-generalized tx
    // sets won't exist in the network anymore.
    void computeTxFeesForNonGeneralizedSet(LedgerHeader const& lclHeader) const;

    bool addTxsFromXdr(Application& app, AbstractLedgerTxn& ltx,
                       xdr::xvector<TransactionEnvelope> const& txs,
                       bool useBaseFee, std::optional<int64_t> baseFee,
                       TxSetFrame::Phase phase);
    void applySurgePricing(Application& app);

    void computeTxFeesForNonGeneralizedSet(LedgerHeader const& lclHeader,
                                           int64_t lowestBaseFee,
                                           bool enableLogging) const;

    void computeTxFees(TxSetFrame::Phase phase,
                       LedgerHeader const& ledgerHeader,
                       SurgePricingLaneConfig const& surgePricingConfig,
                       std::vector<int64_t> const& lowestLaneFee,
                       std::vector<bool> const& hadTxNotFittingLane);
    std::optional<Resource> getTxSetSorobanResource() const;

    // Get _inclusion_ fee map for a given phase. The map contains lowest base
    // fee for each transaction (lowest base fee is identical for all
    // transactions in the same lane)
    std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>&
    getInclusionFeeMap(TxSetFrame::Phase phase) const;

    TxSetFrameConstPtr toWireTxSetFrame() const;
    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;

    bool const mIsGeneralized;

    Hash const mPreviousLedgerHash;
    // There can only be 1 phase (classic) prior to protocol 20.
    // Starting protocol 20, there will be 2 phases (classic and soroban).
    std::vector<TxSetFrame::Transactions> mTxPhases;

    mutable std::vector<bool> mFeesComputed;
    mutable std::vector<std::unordered_map<TransactionFrameBaseConstPtr,
                                           std::optional<int64_t>>>
        mPhaseInclusionFeeMap;

    std::weak_ptr<TxSetFrame const> mParentTxSetFrame{};
#ifdef BUILD_TESTS
    mutable std::optional<TxSetFrame::Transactions> mApplyOrderOverride;
#endif
};

} // namespace stellar
