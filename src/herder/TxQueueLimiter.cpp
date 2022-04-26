// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxQueueLimiter.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{
namespace
{
class SingleTxStack : public TxStack
{
  public:
    SingleTxStack(TransactionFrameBasePtr tx) : mTx(tx)
    {
    }

    TransactionFrameBasePtr
    getTopTx() const override
    {
        releaseAssert(mTx);
        return mTx;
    }

    void
    popTopTx() override
    {
        releaseAssert(mTx);
        mTx = nullptr;
    }

    bool
    empty() const override
    {
        return mTx == nullptr;
    }

    uint32_t
    getNumOperations() const override
    {
        releaseAssert(mTx);
        return mTx->getNumOperations();
    }

  private:
    TransactionFrameBasePtr mTx;
};

int64_t
computeBetterFee(std::pair<int64, uint32_t> const& evictedBid,
                 TransactionFrameBase const& tx)
{
    if (evictedBid.second != 0 &&
        feeRate3WayCompare(evictedBid.first, evictedBid.second, tx.getFeeBid(),
                           tx.getNumOperations()) >= 0)
    {
        return computeBetterFee(tx, evictedBid.first, evictedBid.second);
    }
    return 0;
}

}

TxQueueLimiter::TxQueueLimiter(uint32 multiplier, Application& app)
    : mPoolLedgerMultiplier(multiplier), mLedgerManager(app.getLedgerManager())
{
    auto maxDexOps = app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET;
    if (maxDexOps)
    {
        mMaxDexOperations = *maxDexOps * multiplier;
    }
}

TxQueueLimiter::~TxQueueLimiter()
{
    // empty destructor allows deleting TxQueueLimiter from other source files
}

#ifdef BUILD_TESTS
size_t
TxQueueLimiter::size() const
{
    return mTxs->sizeOps();
}
#endif

uint32_t
TxQueueLimiter::maxQueueSizeOps() const
{
    uint32_t maxOpsLedger = mLedgerManager.getLastMaxTxSetSizeOps();
    maxOpsLedger *= mPoolLedgerMultiplier;
    return maxOpsLedger;
}

void
TxQueueLimiter::addTransaction(TransactionFrameBasePtr const& tx)
{
    auto txStack = std::make_shared<SingleTxStack>(tx);
    mStackForTx[tx] = txStack;
    mTxs->add(txStack);
}

void
TxQueueLimiter::removeTransaction(TransactionFrameBasePtr const& tx)
{
    auto txStackIt = mStackForTx.find(tx);
    if (txStackIt == mStackForTx.end())
    {
        throw std::logic_error(
            "invalid state (missing tx) while removing tx in TxQueueLimiter");
    }
    mTxs->erase(txStackIt->second);
}

std::pair<bool, int64>
TxQueueLimiter::canAddTx(TransactionFrameBasePtr const& newTx,
                         TransactionFrameBasePtr const& oldTx)
{
    // The usage pattern of the limiter is either canAddTx->evictTransactions
    // (where we cleanup the cached txs to evict) or canAddTx->canAddTx
    // (when tx hasn't been added for unrelated reasons). Clear the cache here
    // to address the second pattern.
    mEvictionCache.clear();
    // We cannot normally initialize transaction queue in the constructor
    // because `maxQueueSizeOps()` may not be initialized. Hence we initialize
    // lazily during the add/reset.
    if (mTxs == nullptr)
    {
        resetTxs();
    }

    // If some transactions were evicted from this or generic lane, make sure
    // that the new transaction is better (even if it fits otherwise). This
    // guarantees that we don't replace transactions with higher bids with
    // transactions with lower bids and less operations.
    int64_t minFeeToBeatEvicted = std::max(
        computeBetterFee(
            mLaneEvictedFeeBid[mSurgePricingLaneConfig->getLaneClassifier()(
                *newTx)],
            *newTx),
        computeBetterFee(
            mLaneEvictedFeeBid[SurgePricingPriorityQueue::GENERIC_LANE],
            *newTx));
    if (minFeeToBeatEvicted > 0)
    {
        return std::make_pair(false, minFeeToBeatEvicted);
    }

    uint32_t txOpsDiscount = 0;
    if (oldTx)
    {
        auto newTxOps = newTx->getNumOperations();
        auto oldTxOps = oldTx->getNumOperations();
        // Bump transactions must have at least the old amount of operations.
        releaseAssert(oldTxOps <= newTxOps);
        txOpsDiscount = newTxOps - oldTxOps;
    }
    return mTxs->canFitWithEviction(*newTx, txOpsDiscount, mEvictionCache);
}

void
TxQueueLimiter::evictTransactions(
    TransactionFrameBaseConstPtr newTx,
    std::function<void(TransactionFrameBasePtr const&)> evict)
{
    for (auto const& [evictedStack, evictedDueToLaneLimit] : mEvictionCache)
    {
        auto tx = evictedStack->getTopTx();
        if (evictedDueToLaneLimit)
        {
            // If tx has been evicted due to lane limit, then all the following
            // txs in this lane have to beat it. However, other txs could still
            // fit with a lower fee.
            mLaneEvictedFeeBid[mSurgePricingLaneConfig->getLaneClassifier()(
                *tx)] = {tx->getFeeBid(), tx->getNumOperations()};
        }
        else
        {
            // If tx has been evicted before reaching the lane limit, we just
            // add it to generic lane, so that every new tx has to beat it.
            mLaneEvictedFeeBid[SurgePricingPriorityQueue::GENERIC_LANE] = {
                tx->getFeeBid(), tx->getNumOperations()};
        }

        evict(tx);
    }
    mEvictionCache.clear();
}

void
TxQueueLimiter::reset()
{
    resetTxs();
    resetEvictionState();
}

void
TxQueueLimiter::resetEvictionState()
{
    // We cannot normally initialiaze transaction queue in the constructor
    // because `maxQueueSizeOps()` may not be initialized. Hence we initialize
    // lazily during the add/reset.
    if (mTxs == nullptr)
    {
        resetTxs();
    }
    mLaneEvictedFeeBid.assign(mLaneEvictedFeeBid.size(), {0, 0});
}

void
TxQueueLimiter::resetTxs()
{
    mSurgePricingLaneConfig = std::make_shared<DexLimitingLaneConfig>(
        maxQueueSizeOps(), mMaxDexOperations);
    mTxs = std::make_unique<SurgePricingPriorityQueue>(
        /* isHighestPriority */ false, mSurgePricingLaneConfig,
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));
    mLaneEvictedFeeBid.resize(
        mSurgePricingLaneConfig->getLaneOpsLimits().size());
}
}
