// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TxSetFrame.h"
#include "TxSetUtils.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include <Tracy.hpp>
#include <algorithm>
#include <list>
#include <numeric>
#include <variant>

namespace stellar
{

namespace
{
bool
validateTxSetXDRStructure(GeneralizedTransactionSet const& txSet)
{
    if (txSet.v() != 1)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: unsupported version {}", txSet.v());
        return false;
    }
    auto const& txSetV1 = txSet.v1TxSet();
    // There was no protocol with 1 phase, so checking for 2 phases only
    if (txSetV1.phases.size() !=
        static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT))
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet: exactly 2 phases are expected, got {}",
                   txSetV1.phases.size());
        return false;
    }

    for (auto const& phase : txSetV1.phases)
    {
        if (phase.v() != 0)
        {
            CLOG_DEBUG(Herder, "Got bad txSet: unsupported phase version {}",
                       phase.v());
            return false;
        }

        bool componentsNormalized = std::is_sorted(
            phase.v0Components().begin(), phase.v0Components().end(),
            [](auto const& c1, auto const& c2) {
                if (!c1.txsMaybeDiscountedFee().baseFee ||
                    !c2.txsMaybeDiscountedFee().baseFee)
                {
                    return !c1.txsMaybeDiscountedFee().baseFee &&
                           c2.txsMaybeDiscountedFee().baseFee;
                }
                return *c1.txsMaybeDiscountedFee().baseFee <
                       *c2.txsMaybeDiscountedFee().baseFee;
            });
        if (!componentsNormalized)
        {
            CLOG_DEBUG(Herder, "Got bad txSet: incorrect component order");
            return false;
        }

        bool componentBaseFeesUnique =
            std::adjacent_find(
                phase.v0Components().begin(), phase.v0Components().end(),
                [](auto const& c1, auto const& c2) {
                    if (!c1.txsMaybeDiscountedFee().baseFee ||
                        !c2.txsMaybeDiscountedFee().baseFee)
                    {
                        return !c1.txsMaybeDiscountedFee().baseFee &&
                               !c2.txsMaybeDiscountedFee().baseFee;
                    }
                    return *c1.txsMaybeDiscountedFee().baseFee ==
                           *c2.txsMaybeDiscountedFee().baseFee;
                }) == phase.v0Components().end();
        if (!componentBaseFeesUnique)
        {
            CLOG_DEBUG(Herder, "Got bad txSet: duplicate component base fees");
            return false;
        }
        for (auto const& component : phase.v0Components())
        {
            if (component.txsMaybeDiscountedFee().txs.empty())
            {
                CLOG_DEBUG(Herder, "Got bad txSet: empty component");
                return false;
            }
        }
    }
    return true;
}
// We want to XOR the tx hash with the set hash.
// This way people can't predict the order that txs will be applied in
struct ApplyTxSorter
{
    Hash mSetHash;
    ApplyTxSorter(Hash h) : mSetHash{std::move(h)}
    {
    }

    bool
    operator()(TransactionFrameBasePtr const& tx1,
               TransactionFrameBasePtr const& tx2) const
    {
        // need to use the hash of whole tx here since multiple txs could have
        // the same Contents
        return lessThanXored(tx1->getFullHash(), tx2->getFullHash(), mSetHash);
    }
};

Hash
computeNonGenericTxSetContentsHash(TransactionSet const& xdrTxSet)
{
    ZoneScoped;
    SHA256 hasher;
    hasher.add(xdrTxSet.previousLedgerHash);
    for (auto const& tx : xdrTxSet.txs)
    {
        hasher.add(xdr::xdr_to_opaque(tx));
    }
    return hasher.finish();
}

// Note: Soroban txs also use this functionality for simplicity, as it's a no-op
// (all Soroban txs have 1 op max)
int64_t
computePerOpFee(TransactionFrameBase const& tx, uint32_t ledgerVersion)
{
    auto rounding =
        protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION)
            ? Rounding::ROUND_DOWN
            : Rounding::ROUND_UP;
    auto txOps = tx.getNumOperations();
    return bigDivideOrThrow(tx.getInclusionFee(), 1,
                            static_cast<int64_t>(txOps), rounding);
}

} // namespace

TxSetFrame::TxSetFrame(TransactionSet const& xdrTxSet)
    : mXDRTxSet(xdrTxSet)
    , mEncodedSize(xdr::xdr_argpack_size(xdrTxSet))
    , mHash(computeNonGenericTxSetContentsHash(xdrTxSet))
{
}

TxSetFrame::TxSetFrame(GeneralizedTransactionSet const& xdrTxSet)
    : mXDRTxSet(xdrTxSet)
    , mEncodedSize(xdr::xdr_argpack_size(xdrTxSet))
    , mHash(xdrSha256(xdrTxSet))
{
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(TransactionSet const& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(xdrTxSet));
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromWire(GeneralizedTransactionSet const& xdrTxSet)
{
    ZoneScoped;
    std::shared_ptr<TxSetFrame> txSet(new TxSetFrame(xdrTxSet));
    return txSet;
}

TxSetFrameConstPtr
TxSetFrame::makeFromStoredTxSet(StoredTransactionSet const& storedSet)
{
    if (storedSet.v() == 0)
    {
        return TxSetFrame::makeFromWire(storedSet.txSet());
    }
    return TxSetFrame::makeFromWire(storedSet.generalizedTxSet());
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxSetFrame::TxPhases const& txPhases,
                                 Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset
#ifdef BUILD_TESTS
                                 ,
                                 bool skipValidation
#endif
)
{
    TxPhases invalidTxs;
    invalidTxs.resize(txPhases.size());
    return makeFromTransactions(txPhases, app, lowerBoundCloseTimeOffset,
                                upperBoundCloseTimeOffset, invalidTxs
#ifdef BUILD_TESTS
                                ,
                                skipValidation
#endif
    );
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxSetFrame::TxPhases const& txPhases,
                                 Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 TxSetFrame::TxPhases& invalidTxs
#ifdef BUILD_TESTS
                                 ,
                                 bool skipValidation
#endif
)
{
    releaseAssert(txPhases.size() == invalidTxs.size());
    releaseAssert(txPhases.size() <=
                  static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT));

    TxSetFrame::TxPhases validatedPhases;
    for (int i = 0; i < txPhases.size(); ++i)
    {
        auto& txs = txPhases[i];
        bool expectSoroban =
            static_cast<TxSetFrame::Phase>(i) == TxSetFrame::Phase::SOROBAN;
        if (!std::all_of(txs.begin(), txs.end(), [&](auto const& tx) {
                return tx->isSoroban() == expectSoroban;
            }))
        {
            throw std::runtime_error("TxSetFrame::makeFromTransactions: phases "
                                     "contain txs of wrong type");
        }

        auto& invalid = invalidTxs[i];
#ifdef BUILD_TESTS
        if (skipValidation)
        {
            validatedPhases.emplace_back(txs);
        }
        else
        {
#endif
            validatedPhases.emplace_back(
                TxSetUtils::trimInvalid(txs, app, lowerBoundCloseTimeOffset,
                                        upperBoundCloseTimeOffset, invalid));
#ifdef BUILD_TESTS
        }
#endif
    }

    auto const& lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    ResolvedTxSetFrame resolvedTxSet(lclHeader, validatedPhases);
    resolvedTxSet.applySurgePricing(app);
    auto outputTxSet = resolvedTxSet.toWireTxSetFrame();
    // Do the roundtrip through XDR to ensure we never build an incorrect tx set
    // for nomination.

#ifdef BUILD_TESTS
    if (skipValidation)
    {
        resolvedTxSet.mParentTxSetFrame = outputTxSet;
        outputTxSet->mResolvedTxSet = std::unique_ptr<ResolvedTxSetFrame>(
            new ResolvedTxSetFrame(resolvedTxSet));
        return outputTxSet;
    }
#endif

    ResolvedTxSetFrame const* resolvedOutputTxSet = nullptr;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot(), false,
                      TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
        resolvedOutputTxSet = outputTxSet->resolve(app, ltx);
    }
    if (!resolvedOutputTxSet)
    {
        throw std::runtime_error("Couldn't resolve created tx set frame");
    }

    // Make sure no transactions were lost during the roundtrip and the output
    // tx set is valid.
    bool valid = resolvedTxSet.numPhases() == resolvedOutputTxSet->numPhases();
    if (valid)
    {
        for (int i = 0; i < resolvedTxSet.numPhases(); ++i)
        {
            valid =
                valid && resolvedTxSet.sizeTx(static_cast<Phase>(i)) ==
                             resolvedOutputTxSet->sizeTx(static_cast<Phase>(i));
        }
    }

    valid =
        valid && resolvedOutputTxSet->checkValid(app, lowerBoundCloseTimeOffset,
                                                 upperBoundCloseTimeOffset);
    if (!valid)
    {
        throw std::runtime_error("Created invalid tx set frame");
    }
    return outputTxSet;
}

TxSetFrameConstPtr
TxSetFrame::makeEmpty(LedgerHeaderHistoryEntry const& lclHeader)
{
    TxPhases phases;
    phases.resize(protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                            SOROBAN_PROTOCOL_VERSION)
                      ? static_cast<size_t>(Phase::PHASE_COUNT)
                      : 1);
    ResolvedTxSetFrame txSet(lclHeader, phases);
    for (int i = 0; i < txSet.mFeesComputed.size(); i++)
    {
        txSet.mFeesComputed[i] = true;
    }
    return txSet.toWireTxSetFrame();
}

TxSetFrameConstPtr
TxSetFrame::makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                        TxSetFrame::Transactions const& txs)
{
    ResolvedTxSetFrame resolvedTxSet(false, previousLedgerHash,
                                     TxSetFrame::TxPhases{txs});
    return resolvedTxSet.toWireTxSetFrame();
}

#ifdef BUILD_TESTS
TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(TxSetFrame::Transactions txs, Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 bool enforceTxsApplyOrder)
{
    Transactions invalid;
    return TxSetFrame::makeFromTransactions(txs, app, lowerBoundCloseTimeOffset,
                                            upperBoundCloseTimeOffset, invalid,
                                            enforceTxsApplyOrder);
}

TxSetFrameConstPtr
TxSetFrame::makeFromTransactions(Transactions txs, Application& app,
                                 uint64_t lowerBoundCloseTimeOffset,
                                 uint64_t upperBoundCloseTimeOffset,
                                 Transactions& invalidTxs,
                                 bool enforceTxsApplyOrder)
{
    TxSetFrame::TxPhases phases;
    phases.emplace_back(txs);
    auto lclHeader = app.getLedgerManager().getLastClosedLedgerHeader();
    if (protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION))
    {
        // Empty soroban phase
        phases.emplace_back();
    }
    TxSetFrame::TxPhases invalid;
    invalid.resize(phases.size());
    auto res = TxSetFrame::makeFromTransactions(
        phases, app, lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset,
        invalid, enforceTxsApplyOrder);
    if (enforceTxsApplyOrder)
    {
        (*res->mResolvedTxSet)->mApplyOrderOverride = txs;
    }
    invalidTxs = invalid[0];
    return res;
}
#endif

ResolvedTxSetFrame const*
TxSetFrame::resolve(Application& app, AbstractLedgerTxn& ltx) const
{
    if (mResolvedTxSet)
    {
        return mResolvedTxSet->get();
    }
    ZoneScoped;
    releaseAssert(previousLedgerHash() ==
                  app.getLedgerManager().getLastClosedLedgerHeader().hash);

    if (isGeneralizedTxSet())
    {
        auto const& xdrTxSet = std::get<GeneralizedTransactionSet>(mXDRTxSet);
        if (!validateTxSetXDRStructure(xdrTxSet))
        {
            CLOG_DEBUG(Herder,
                       "Got bad generalized txSet with invalid XDR structure");
            return nullptr;
        }
        auto const& phases = xdrTxSet.v1TxSet().phases;
        TxSetFrame::TxPhases defaultPhases;
        defaultPhases.resize(phases.size());

        std::unique_ptr<ResolvedTxSetFrame> txSet(new ResolvedTxSetFrame(
            true, xdrTxSet.v1TxSet().previousLedgerHash, defaultPhases));
        // Mark fees as already computed as we read them from the XDR.
        for (int i = 0; i < txSet->mFeesComputed.size(); i++)
        {
            txSet->mFeesComputed[i] = true;
        }

        releaseAssert(phases.size() <=
                      static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT));
        for (auto phaseId = 0; phaseId < phases.size(); phaseId++)
        {
            auto const& phase = phases[phaseId];
            auto const& components = phase.v0Components();
            for (auto const& component : components)
            {
                switch (component.type())
                {
                case TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE:
                    std::optional<int64_t> baseFee;
                    if (component.txsMaybeDiscountedFee().baseFee)
                    {
                        baseFee = *component.txsMaybeDiscountedFee().baseFee;
                    }
                    if (!txSet->addTxsFromXdr(
                            app, ltx, component.txsMaybeDiscountedFee().txs,
                            true, baseFee,
                            static_cast<TxSetFrame::Phase>(phaseId)))
                    {
                        CLOG_DEBUG(Herder,
                                   "Got bad txSet: transactions are not "
                                   "ordered correctly or contain invalid phase "
                                   "transactions");
                        mResolvedTxSet = std::make_optional(nullptr);
                        return nullptr;
                    }
                    break;
                }
            }
        }
        mResolvedTxSet = std::make_optional(std::move(txSet));
    }
    else
    {
        auto const& xdrTxSet = std::get<TransactionSet>(mXDRTxSet);
        std::unique_ptr<ResolvedTxSetFrame> txSet(new ResolvedTxSetFrame(
            false, xdrTxSet.previousLedgerHash, {TxSetFrame::Transactions{}}));
        size_t encodedSize = xdr::xdr_argpack_size(xdrTxSet);
        if (!txSet->addTxsFromXdr(app, ltx, xdrTxSet.txs, false, std::nullopt,
                                  TxSetFrame::Phase::CLASSIC))
        {
            CLOG_DEBUG(
                Herder,
                "Got bad txSet: transactions are not "
                "ordered correctly or contain invalid phase transactions");
            mResolvedTxSet = std::make_optional(nullptr);
            return nullptr;
        }
        mResolvedTxSet = std::make_optional(std::move(txSet));
    }
    // NB: `TxSetFrame` is only owned externally as
    // `std::shared_ptr<TxSetFrame const>`, thus it's safe to use
    // `weak_from_this`.
    (*mResolvedTxSet)->mParentTxSetFrame = weak_from_this();
    return mResolvedTxSet->get();
}

ResolvedTxSetFrame const*
TxSetFrame::getResolvedFrame() const
{
    if (!mResolvedTxSet)
    {
        return nullptr;
    }
    return mResolvedTxSet->get();
}

bool
TxSetFrame::isGeneralizedTxSet() const
{
    return std::holds_alternative<GeneralizedTransactionSet>(mXDRTxSet);
}

Hash const&
TxSetFrame::getContentsHash() const
{
    return mHash;
}

Hash const&
TxSetFrame::previousLedgerHash() const
{
    if (isGeneralizedTxSet())
    {
        return std::get<GeneralizedTransactionSet>(mXDRTxSet)
            .v1TxSet()
            .previousLedgerHash;
    }
    return std::get<TransactionSet>(mXDRTxSet).previousLedgerHash;
}

size_t
TxSetFrame::sizeTxTotal() const
{
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        size_t totalSize = 0;
        for (auto const& phase : txSet.phases)
        {
            for (auto const& component : phase.v0Components())
            {
                totalSize += component.txsMaybeDiscountedFee().txs.size();
            }
        }
        return totalSize;
    }
    else
    {
        return std::get<TransactionSet>(mXDRTxSet).txs.size();
    }
}

size_t
TxSetFrame::sizeOpTotal() const
{
    auto accumulateTxsFn = [](size_t sz, TransactionEnvelope const& tx) {
        size_t txOps = 0;
        if (tx.type() == 0)
        {
            txOps = tx.v0().tx.operations.size();
        }
        else
        {
            txOps = tx.v1().tx.operations.size();
        }
        return sz + txOps;
    };
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        size_t totalSize = 0;
        for (auto const& phase : txSet.phases)
        {
            for (auto const& component : phase.v0Components())
            {
                totalSize += std::accumulate(
                    component.txsMaybeDiscountedFee().txs.begin(),
                    component.txsMaybeDiscountedFee().txs.end(), 0,
                    accumulateTxsFn);
            }
        }
        return totalSize;
    }
    else
    {
        auto const& txs = std::get<TransactionSet>(mXDRTxSet).txs;
        return std::accumulate(txs.begin(), txs.end(), 0, accumulateTxsFn);
    }
}

TxSetFrame::Transactions
TxSetFrame::createTransactionFrames(Hash const& networkID) const
{
    TxSetFrame::Transactions txs;
    if (isGeneralizedTxSet())
    {
        auto const& txSet =
            std::get<GeneralizedTransactionSet>(mXDRTxSet).v1TxSet();
        for (auto const& phase : txSet.phases)
        {
            for (auto const& component : phase.v0Components())
            {
                for (auto const& tx : component.txsMaybeDiscountedFee().txs)
                {
                    txs.emplace_back(
                        TransactionFrameBase::makeTransactionFromWire(networkID,
                                                                      tx));
                }
            }
        }
    }
    else
    {
        auto const& txSet = std::get<TransactionSet>(mXDRTxSet).txs;
        for (auto const& tx : txSet)
        {
            txs.emplace_back(
                TransactionFrameBase::makeTransactionFromWire(networkID, tx));
        }
    }
    return txs;
}

size_t
TxSetFrame::encodedSize() const
{
    return mEncodedSize;
}

void
TxSetFrame::toXDR(TransactionSet& txSet) const
{
    releaseAssert(!isGeneralizedTxSet());
    txSet = std::get<TransactionSet>(mXDRTxSet);
}

void
TxSetFrame::toXDR(GeneralizedTransactionSet& txSet) const
{
    releaseAssert(isGeneralizedTxSet());
    txSet = std::get<GeneralizedTransactionSet>(mXDRTxSet);
}

void
TxSetFrame::storeXDR(StoredTransactionSet& txSet) const
{
    if (isGeneralizedTxSet())
    {
        txSet.v(1);
        txSet.generalizedTxSet() =
            std::get<GeneralizedTransactionSet>(mXDRTxSet);
    }
    else
    {
        txSet.v(0);
        txSet.txSet() = std::get<TransactionSet>(mXDRTxSet);
    }
}

ResolvedTxSetFrame::ResolvedTxSetFrame(bool isGeneralized,
                                       Hash const& previousLedgerHash,
                                       TxSetFrame::TxPhases const& txs)
    : mIsGeneralized(isGeneralized)
    , mPreviousLedgerHash(previousLedgerHash)
    , mTxPhases(txs)
{
    mFeesComputed.resize(mTxPhases.size(), false);
}

ResolvedTxSetFrame::ResolvedTxSetFrame(
    LedgerHeaderHistoryEntry const& lclHeader, TxSetFrame::TxPhases const& txs)
    : ResolvedTxSetFrame(
          protocolVersionStartsFrom(lclHeader.header.ledgerVersion,
                                    SOROBAN_PROTOCOL_VERSION),
          lclHeader.hash, txs)
{
}

Hash const&
ResolvedTxSetFrame::getContentsHash() const
{
    auto parentFrame = mParentTxSetFrame.lock();
    releaseAssert(parentFrame);
    return parentFrame->getContentsHash();
}

TxSetFrame::Transactions const&
ResolvedTxSetFrame::getTxsForPhase(TxSetFrame::Phase phase) const
{
    releaseAssert(static_cast<size_t>(phase) < mTxPhases.size());
    return mTxPhases.at(static_cast<size_t>(phase));
}

TxSetFrame::Transactions
ResolvedTxSetFrame::getTxsInApplyOrder() const
{
#ifdef BUILD_TESTS
    if (mApplyOrderOverride)
    {
        return *mApplyOrderOverride;
    }
#endif
    ZoneScoped;

    // Use a single vector to order transactions from all phases
    std::vector<TransactionFrameBasePtr> retList;
    retList.reserve(sizeTxTotal());

    for (auto const& phase : mTxPhases)
    {
        auto txQueues = TxSetUtils::buildAccountTxQueues(phase);

        // build txBatches
        // txBatches i-th element contains each i-th transaction for accounts
        // with a transaction in the transaction set
        std::vector<std::vector<TransactionFrameBasePtr>> txBatches;

        while (!txQueues.empty())
        {
            txBatches.emplace_back();
            auto& curBatch = txBatches.back();
            // go over all users that still have transactions
            for (auto it = txQueues.begin(); it != txQueues.end();)
            {
                auto& txQueue = *it;
                curBatch.emplace_back(txQueue->getTopTx());
                txQueue->popTopTx();
                if (txQueue->empty())
                {
                    // done with that user
                    it = txQueues.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (auto& batch : txBatches)
        {
            // randomize each batch using the hash of the transaction set
            // as a way to randomize even more
            ApplyTxSorter s(getContentsHash());
            std::sort(batch.begin(), batch.end(), s);
            for (auto const& tx : batch)
            {
                retList.push_back(tx);
            }
        }
    }

    return retList;
}

// need to make sure every account that is submitting a tx has enough to pay
// the fees of all the tx it has submitted in this set
// check seq num
bool
ResolvedTxSetFrame::checkValid(Application& app,
                               uint64_t lowerBoundCloseTimeOffset,
                               uint64_t upperBoundCloseTimeOffset) const
{
    ZoneScoped;
    auto& lcl = app.getLedgerManager().getLastClosedLedgerHeader();

    // Start by checking previousLedgerHash
    if (lcl.hash != mPreviousLedgerHash)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: {}, expected {}",
                   hexAbbrev(mPreviousLedgerHash), hexAbbrev(lcl.hash));
        return false;
    }

    bool needGeneralizedTxSet = protocolVersionStartsFrom(
        lcl.header.ledgerVersion, SOROBAN_PROTOCOL_VERSION);
    if (needGeneralizedTxSet != isGeneralizedTxSet())
    {
        CLOG_DEBUG(Herder,
                   "Got bad txSet {}: need generalized '{}', expected '{}'",
                   hexAbbrev(mPreviousLedgerHash), needGeneralizedTxSet,
                   isGeneralizedTxSet());
        return false;
    }

    if (isGeneralizedTxSet())
    {
        releaseAssert(std::all_of(mFeesComputed.begin(), mFeesComputed.end(),
                                  [](bool comp) { return comp; }));
        auto checkFeeMap = [&](auto const& feeMap) {
            for (auto const& [tx, fee] : feeMap)
            {
                if (!fee)
                {
                    continue;
                }
                if (*fee < lcl.header.baseFee)
                {

                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: {} has too low component base fee {}",
                        hexAbbrev(mPreviousLedgerHash), *fee);
                    return false;
                }
                if (tx->getInclusionFee() <
                    getMinInclusionFee(*tx, lcl.header, fee))
                {
                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: {} has tx with fee bid ({}) lower "
                        "than base fee ({})",
                        hexAbbrev(mPreviousLedgerHash), tx->getInclusionFee(),
                        getMinInclusionFee(*tx, lcl.header, fee));
                    return false;
                }
            }
            return true;
        };

        if (!checkFeeMap(getInclusionFeeMap(TxSetFrame::Phase::CLASSIC)))
        {
            return false;
        }
        if (!checkFeeMap(getInclusionFeeMap(TxSetFrame::Phase::SOROBAN)))
        {
            return false;
        }
    }

    if (this->size(lcl.header, TxSetFrame::Phase::CLASSIC) >
        lcl.header.maxTxSetSize)
    {
        CLOG_DEBUG(Herder, "Got bad txSet: too many classic txs {} > {}",
                   this->size(lcl.header, TxSetFrame::Phase::CLASSIC),
                   lcl.header.maxTxSetSize);
        return false;
    }

    if (needGeneralizedTxSet)
    {
        // First, ensure the tx set does not contain multiple txs per source
        // account
        std::unordered_set<AccountID> seenAccounts;
        for (auto const& phase : mTxPhases)
        {
            for (auto const& tx : phase)
            {
                if (!seenAccounts.insert(tx->getSourceID()).second)
                {
                    CLOG_DEBUG(
                        Herder,
                        "Got bad txSet: multiple txs per source account");
                    return false;
                }
            }
        }

        // Second, ensure total resources are not over ledger limit
        auto totalTxSetRes = getTxSetSorobanResource();
        if (!totalTxSetRes)
        {
            CLOG_DEBUG(Herder,
                       "Got bad txSet: total Soroban resources overflow");
            return false;
        }

        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            auto limits = app.getLedgerManager().maxLedgerResources(
                /* isSoroban */ true, ltx);
            if (anyGreater(*totalTxSetRes, limits))
            {
                CLOG_DEBUG(Herder,
                           "Got bad txSet: needed resources exceed ledger "
                           "limits {} > {}",
                           totalTxSetRes->toString(), limits.toString());
                return false;
            }
        }
    }

    bool allValid = true;
    for (auto const& txs : mTxPhases)
    {
        if (!TxSetUtils::getInvalidTxList(txs, app, lowerBoundCloseTimeOffset,
                                          upperBoundCloseTimeOffset, true)
                 .empty())
        {
            allValid = false;
            break;
        }
    }
    return allValid;
}

size_t
ResolvedTxSetFrame::size(LedgerHeader const& lh,
                         std::optional<TxSetFrame::Phase> phase) const
{
    size_t sz = 0;
    if (!phase)
    {
        if (numPhases() > static_cast<size_t>(TxSetFrame::Phase::SOROBAN))
        {
            sz += sizeOp(TxSetFrame::Phase::SOROBAN);
        }
    }
    else if (phase.value() == TxSetFrame::Phase::SOROBAN)
    {
        sz += sizeOp(TxSetFrame::Phase::SOROBAN);
    }
    if (!phase || phase.value() == TxSetFrame::Phase::CLASSIC)
    {
        sz += protocolVersionStartsFrom(lh.ledgerVersion, ProtocolVersion::V_11)
                  ? sizeOp(TxSetFrame::Phase::CLASSIC)
                  : sizeTx(TxSetFrame::Phase::CLASSIC);
    }
    return sz;
}

size_t
ResolvedTxSetFrame::sizeOp(TxSetFrame::Phase phase) const
{
    ZoneScoped;
    auto const& txs = mTxPhases.at(static_cast<size_t>(phase));
    return std::accumulate(txs.begin(), txs.end(), size_t(0),
                           [&](size_t a, TransactionFrameBasePtr const& tx) {
                               return a + tx->getNumOperations();
                           });
}

size_t
ResolvedTxSetFrame::sizeOpTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        total += sizeOp(static_cast<TxSetFrame::Phase>(i));
    }
    return total;
}

size_t
ResolvedTxSetFrame::sizeTxTotal() const
{
    ZoneScoped;
    size_t total = 0;
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        total += sizeTx(static_cast<TxSetFrame::Phase>(i));
    }
    return total;
}

size_t
ResolvedTxSetFrame::encodedSize() const
{
    auto parentFrame = mParentTxSetFrame.lock();
    releaseAssert(parentFrame);
    return parentFrame->encodedSize();
}

void
ResolvedTxSetFrame::computeTxFeesForNonGeneralizedSet(
    LedgerHeader const& lclHeader) const
{
    ZoneScoped;
    auto ledgerVersion = lclHeader.ledgerVersion;
    int64_t lowBaseFee = std::numeric_limits<int64_t>::max();
    releaseAssert(mTxPhases.size() == 1);
    for (auto& txPtr : mTxPhases[0])
    {
        int64_t txBaseFee = computePerOpFee(*txPtr, ledgerVersion);
        lowBaseFee = std::min(lowBaseFee, txBaseFee);
    }
    computeTxFeesForNonGeneralizedSet(lclHeader, lowBaseFee,
                                      /* enableLogging */ false);
}

void
ResolvedTxSetFrame::computeTxFeesForNonGeneralizedSet(
    LedgerHeader const& lclHeader, int64_t lowestBaseFee,
    bool enableLogging) const
{
    ZoneScoped;
    releaseAssert(std::none_of(mFeesComputed.begin(), mFeesComputed.end(),
                               [](bool comp) { return comp; }));
    int64_t baseFee = lclHeader.baseFee;

    if (protocolVersionStartsFrom(lclHeader.ledgerVersion,
                                  ProtocolVersion::V_11))
    {
        size_t surgeOpsCutoff = 0;
        if (lclHeader.maxTxSetSize >= MAX_OPS_PER_TX)
        {
            surgeOpsCutoff = lclHeader.maxTxSetSize - MAX_OPS_PER_TX;
        }
        if (sizeOp(TxSetFrame::Phase::CLASSIC) > surgeOpsCutoff)
        {
            baseFee = lowestBaseFee;
            if (enableLogging)
            {
                CLOG_WARNING(Herder, "surge pricing in effect! {} > {}",
                             sizeOp(TxSetFrame::Phase::CLASSIC),
                             surgeOpsCutoff);
            }
        }
    }

    releaseAssert(mTxPhases.size() == 1);
    auto const& phase = mTxPhases[0];

    for (auto const& tx : phase)
    {
        mTxBaseInclusionFeeClassic[tx] = baseFee;
    }
    mFeesComputed[0] = true;
}

void
ResolvedTxSetFrame::computeTxFees(
    TxSetFrame::Phase phase, LedgerHeader const& ledgerHeader,
    SurgePricingLaneConfig const& surgePricingConfig,
    std::vector<int64_t> const& lowestLaneFee,
    std::vector<bool> const& hadTxNotFittingLane)
{
    releaseAssert(!mFeesComputed[phase]);
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(lowestLaneFee.size() == hadTxNotFittingLane.size());
    std::vector<int64_t> laneBaseFee(lowestLaneFee.size(),
                                     ledgerHeader.baseFee);
    auto minBaseFee =
        *std::min_element(lowestLaneFee.begin(), lowestLaneFee.end());
    for (size_t lane = 0; lane < laneBaseFee.size(); ++lane)
    {
        // If generic lane is full, then any transaction had to compete with not
        // included transactions and independently of the lane they need to have
        // at least the minimum fee in the tx set applied.
        if (hadTxNotFittingLane[SurgePricingPriorityQueue::GENERIC_LANE])
        {
            laneBaseFee[lane] = minBaseFee;
        }
        // If limited lane is full, then the transactions in this lane also had
        // to compete with each other and have a base fee associated with this
        // lane only.
        if (lane != SurgePricingPriorityQueue::GENERIC_LANE &&
            hadTxNotFittingLane[lane])
        {
            laneBaseFee[lane] = lowestLaneFee[lane];
        }
        if (laneBaseFee[lane] > ledgerHeader.baseFee)
        {
            CLOG_WARNING(
                Herder,
                "{} phase: surge pricing for '{}' lane is in effect with base "
                "fee={}, baseFee={}",
                TxSetFrame::getPhaseName(phase),
                lane == SurgePricingPriorityQueue::GENERIC_LANE ? "generic"
                                                                : "DEX",
                laneBaseFee[lane], ledgerHeader.baseFee);
        }
    }

    auto const& txs = mTxPhases.at(phase);
    auto& feeMap = getInclusionFeeMap(phase);
    for (auto const& tx : txs)
    {
        feeMap[tx] = laneBaseFee[surgePricingConfig.getLane(*tx)];
    }
    mFeesComputed[phase] = true;
}

std::optional<int64_t>
ResolvedTxSetFrame::getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                 LedgerHeader const& lclHeader) const
{
    if (std::any_of(mFeesComputed.begin(), mFeesComputed.end(),
                    [](bool comp) { return !comp; }))
    {
        releaseAssert(!isGeneralizedTxSet());
        computeTxFeesForNonGeneralizedSet(lclHeader);
    }
    auto it = mTxBaseInclusionFeeClassic.find(tx);
    if (it == mTxBaseInclusionFeeClassic.end())
    {
        it = mTxBaseInclusionFeeSoroban.find(tx);
        if (it == mTxBaseInclusionFeeSoroban.end())
        {
            throw std::runtime_error("Transaction not found in tx set");
        }
    }
    return it->second;
}

std::optional<Resource>
ResolvedTxSetFrame::getTxSetSorobanResource() const
{
    releaseAssert(mTxPhases.size() >
                  static_cast<size_t>(TxSetFrame::Phase::SOROBAN));
    auto total = Resource::makeEmpty(/* isSoroban */ true);
    for (auto const& tx :
         mTxPhases[static_cast<size_t>(TxSetFrame::Phase::SOROBAN)])
    {
        if (total.canAdd(tx->getResources()))
        {
            total += tx->getResources();
        }
        else
        {
            return std::nullopt;
        }
    }
    return std::make_optional<Resource>(total);
}

int64_t
ResolvedTxSetFrame::getTotalFees(LedgerHeader const& lh) const
{
    ZoneScoped;
    int64_t total{0};
    std::for_each(mTxPhases.begin(), mTxPhases.end(),
                  [&](TxSetFrame::Transactions const& phase) {
                      total += std::accumulate(
                          phase.begin(), phase.end(), int64_t(0),
                          [&](int64_t t, TransactionFrameBasePtr const& tx) {
                              return t +
                                     tx->getFee(lh, getTxBaseFee(tx, lh), true);
                          });
                  });

    return total;
}

int64_t
ResolvedTxSetFrame::getTotalInclusionFees() const
{
    ZoneScoped;
    int64_t total{0};
    std::for_each(mTxPhases.begin(), mTxPhases.end(),
                  [&](TxSetFrame::Transactions const& phase) {
                      total += std::accumulate(
                          phase.begin(), phase.end(), int64_t(0),
                          [&](int64_t t, TransactionFrameBasePtr const& tx) {
                              return t + tx->getInclusionFee();
                          });
                  });

    return total;
}

std::string
ResolvedTxSetFrame::summary() const
{
    if (empty())
    {
        return "empty tx set";
    }
    if (isGeneralizedTxSet())
    {
        auto feeStats = [&](auto const& feeMap) {
            std::map<std::optional<int64_t>, std::pair<int, int>>
                componentStats;
            for (auto const& [tx, fee] : feeMap)
            {
                ++componentStats[fee].first;
                componentStats[fee].second += tx->getNumOperations();
            }
            std::string res = fmt::format(FMT_STRING("{} component(s): ["),
                                          componentStats.size());

            for (auto const& [fee, stats] : componentStats)
            {
                if (fee != componentStats.begin()->first)
                {
                    res += ", ";
                }
                if (fee)
                {
                    res += fmt::format(
                        FMT_STRING(
                            "{{discounted txs:{}, ops:{}, base_fee:{}}}"),
                        stats.first, stats.second, *fee);
                }
                else
                {
                    res += fmt::format(
                        FMT_STRING("{{non-discounted txs:{}, ops:{}}}"),
                        stats.first, stats.second);
                }
            }
            res += "]";
            return res;
        };

        std::string status;
        releaseAssert(mTxPhases.size() <=
                      static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT));
        for (auto i = 0; i != mTxPhases.size(); i++)
        {
            if (!status.empty())
            {
                status += ", ";
            }
            status += fmt::format(
                FMT_STRING("{} phase: {}"),
                TxSetFrame::getPhaseName(static_cast<TxSetFrame::Phase>(i)),
                feeStats(
                    getInclusionFeeMap(static_cast<TxSetFrame::Phase>(i))));
        }
        return status;
    }
    else
    {
        return fmt::format(FMT_STRING("txs:{}, ops:{}, base_fee:{}"),
                           sizeTxTotal(), sizeOpTotal(),
                           *mTxBaseInclusionFeeClassic.begin()->second);
    }
}

void
ResolvedTxSetFrame::toXDR(TransactionSet& txSet) const
{
    ZoneScoped;
    releaseAssert(!isGeneralizedTxSet());
    releaseAssert(mTxPhases.size() == 1);
    auto& txs = mTxPhases[0];
    txSet.txs.resize(xdr::size32(txs.size()));
    auto sortedTxs = TxSetUtils::sortTxsInHashOrder(txs);
    for (unsigned int n = 0; n < sortedTxs.size(); n++)
    {
        txSet.txs[n] = sortedTxs[n]->getEnvelope();
    }
    txSet.previousLedgerHash = mPreviousLedgerHash;
}

void
ResolvedTxSetFrame::toXDR(GeneralizedTransactionSet& generalizedTxSet) const
{
    ZoneScoped;
    releaseAssert(isGeneralizedTxSet());
    releaseAssert(std::all_of(mFeesComputed.begin(), mFeesComputed.end(),
                              [](bool comp) { return comp; }));
    releaseAssert(mTxPhases.size() <=
                  static_cast<size_t>(TxSetFrame::Phase::PHASE_COUNT));

    generalizedTxSet.v(1);
    generalizedTxSet.v1TxSet().previousLedgerHash = mPreviousLedgerHash;

    for (int i = 0; i < mTxPhases.size(); ++i)
    {
        auto const& txPhase = mTxPhases[i];
        auto& phase =
            generalizedTxSet.v1TxSet().phases.emplace_back().v0Components();

        auto const& feeMap =
            getInclusionFeeMap(static_cast<TxSetFrame::Phase>(i));
        std::map<std::optional<int64_t>, size_t> feeTxCount;
        for (auto const& [tx, fee] : feeMap)
        {
            ++feeTxCount[fee];
        }
        // Reserve a component per unique base fee in order to have the correct
        // pointers in componentPerBid map.
        phase.reserve(feeTxCount.size());

        std::map<std::optional<int64_t>, xdr::xvector<TransactionEnvelope>*>
            componentPerBid;
        for (auto const& [fee, txCount] : feeTxCount)
        {
            phase.emplace_back(TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            auto& discountedFeeComponent = phase.back().txsMaybeDiscountedFee();
            if (fee)
            {
                discountedFeeComponent.baseFee.activate() = *fee;
            }
            componentPerBid[fee] = &discountedFeeComponent.txs;
            componentPerBid[fee]->reserve(txCount);
        }
        auto sortedTxs = TxSetUtils::sortTxsInHashOrder(txPhase);
        for (auto const& tx : sortedTxs)
        {
            componentPerBid[feeMap.find(tx)->second]->push_back(
                tx->getEnvelope());
        }
    }
}

TxSetFrameConstPtr
ResolvedTxSetFrame::toWireTxSetFrame() const
{
    TxSetFrameConstPtr outputTxSet;
    if (mIsGeneralized)
    {
        GeneralizedTransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        outputTxSet = TxSetFrame::makeFromWire(xdrTxSet);
    }
    else
    {
        TransactionSet xdrTxSet;
        toXDR(xdrTxSet);
        outputTxSet = TxSetFrame::makeFromWire(xdrTxSet);
    }
    return outputTxSet;
}

bool
ResolvedTxSetFrame::isGeneralizedTxSet() const
{
    return mIsGeneralized;
}

bool
ResolvedTxSetFrame::addTxsFromXdr(Application& app, AbstractLedgerTxn& rootLtx,
                                  xdr::xvector<TransactionEnvelope> const& txs,
                                  bool useBaseFee,
                                  std::optional<int64_t> baseFee,
                                  TxSetFrame::Phase phase)
{
    auto& phaseTxs = mTxPhases.at(static_cast<int>(phase));
    size_t oldSize = phaseTxs.size();
    phaseTxs.reserve(oldSize + txs.size());

    LedgerTxn ltx(rootLtx, false, TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;

    for (auto const& env : txs)
    {
        auto tx = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), env);

        if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
        {
            tx->maybeComputeSorobanResourceFee(
                ledgerVersion,
                app.getLedgerManager().getSorobanNetworkConfig(ltx),
                app.getConfig());
        }
        // Phase should be consistent with the tx we're trying to add
        if ((tx->isSoroban() && phase != TxSetFrame::Phase::SOROBAN) ||
            (!tx->isSoroban() && phase != TxSetFrame::Phase::CLASSIC))
        {
            return false;
        }

        phaseTxs.push_back(tx);
        if (useBaseFee)
        {
            getInclusionFeeMap(phase)[tx] = baseFee;
        }
    }
    return std::is_sorted(phaseTxs.begin() + oldSize, phaseTxs.end(),
                          &TxSetUtils::hashTxSorter);
}

void
ResolvedTxSetFrame::applySurgePricing(Application& app)
{
    ZoneScoped;

    if (empty())
    {
        for (int i = 0; i < mFeesComputed.size(); ++i)
        {
            mFeesComputed[i] = true;
        }
        return;
    }

    auto const& lclHeader =
        app.getLedgerManager().getLastClosedLedgerHeader().header;

    releaseAssert(mTxPhases.size() <=
                  static_cast<int>(TxSetFrame::Phase::PHASE_COUNT));
    for (int i = 0; i < mTxPhases.size(); i++)
    {
        TxSetFrame::Phase phaseType = static_cast<TxSetFrame::Phase>(i);
        auto& phase = mTxPhases[i];
        auto actTxQueues = TxSetUtils::buildAccountTxQueues(phase);

        if (phaseType == TxSetFrame::Phase::CLASSIC)
        {
            uint32_t maxOps = static_cast<uint32_t>(
                app.getLedgerManager().getLastMaxTxSetSizeOps());
            std::optional<uint32_t> dexOpsLimit;
            if (isGeneralizedTxSet())
            {
                // DEX operations limit implies that DEX transactions should
                // compete with each other in in a separate fee lane, which is
                // only possible with generalized tx set.
                dexOpsLimit = app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET;
            }

            auto surgePricingLaneConfig =
                std::make_shared<DexLimitingLaneConfig>(maxOps, dexOpsLimit);

            std::vector<bool> hadTxNotFittingLane;
            auto includedTxs =
                SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
                    std::vector<TxStackPtr>(actTxQueues.begin(),
                                            actTxQueues.end()),
                    surgePricingLaneConfig, hadTxNotFittingLane);

            size_t laneCount = surgePricingLaneConfig->getLaneLimits().size();
            std::vector<int64_t> lowestLaneFee(
                laneCount, std::numeric_limits<int64_t>::max());
            for (auto const& tx : includedTxs)
            {
                size_t lane = surgePricingLaneConfig->getLane(*tx);
                auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
                lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
            }

            phase = includedTxs;
            if (isGeneralizedTxSet())
            {
                computeTxFees(TxSetFrame::Phase::CLASSIC, lclHeader,
                              *surgePricingLaneConfig, lowestLaneFee,
                              hadTxNotFittingLane);
            }
            else
            {
                computeTxFeesForNonGeneralizedSet(
                    lclHeader,
                    lowestLaneFee[SurgePricingPriorityQueue::GENERIC_LANE],
                    /* enableLogging */ true);
            }
        }
        else
        {
            releaseAssert(isGeneralizedTxSet());
            releaseAssert(phaseType == TxSetFrame::Phase::SOROBAN);

            LedgerTxn ltx(app.getLedgerTxnRoot(), false,
                          TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);

            auto limits = app.getLedgerManager().maxLedgerResources(
                /* isSoroban */ true, ltx);

            auto surgePricingLaneConfig =
                std::make_shared<SorobanGenericLaneConfig>(limits);

            std::vector<bool> hadTxNotFittingLane;
            auto includedTxs =
                SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
                    std::vector<TxStackPtr>(actTxQueues.begin(),
                                            actTxQueues.end()),
                    surgePricingLaneConfig, hadTxNotFittingLane);

            size_t laneCount = surgePricingLaneConfig->getLaneLimits().size();
            std::vector<int64_t> lowestLaneFee(
                laneCount, std::numeric_limits<int64_t>::max());
            for (auto const& tx : includedTxs)
            {
                size_t lane = surgePricingLaneConfig->getLane(*tx);
                auto perOpFee = computePerOpFee(*tx, lclHeader.ledgerVersion);
                lowestLaneFee[lane] = std::min(lowestLaneFee[lane], perOpFee);
            }

            phase = includedTxs;
            computeTxFees(phaseType, lclHeader, *surgePricingLaneConfig,
                          lowestLaneFee, hadTxNotFittingLane);
        }

        releaseAssert(mFeesComputed[i]);
    }
}

std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>&
ResolvedTxSetFrame::getInclusionFeeMap(TxSetFrame::Phase phase) const
{
    if (phase == TxSetFrame::Phase::SOROBAN)
    {
        return mTxBaseInclusionFeeSoroban;
    }
    releaseAssert(phase == TxSetFrame::Phase::CLASSIC);
    return mTxBaseInclusionFeeClassic;
}

std::string
TxSetFrame::getPhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::CLASSIC:
        return "classic";
    case Phase::SOROBAN:
        return "soroban";
    default:
        throw std::runtime_error("Unknown phase");
    }
}
} // namespace stellar
