#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerManager.h"
#include "transactions/TransactionFrame.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"

namespace stellar
{

class TxQueueLimiter
{
    // number of ledgers we can pool in memory
    uint32 const mPoolLedgerMultiplier;
    LedgerManager& mLedgerManager;

    std::unordered_map<TransactionFrameBasePtr, TxStackPtr> mStackForTx;

    // all known transactions
    std::unique_ptr<SurgePricingPriorityQueue> mTxs;

    std::optional<uint32_t> mMaxDexOperations;

    std::vector<std::pair<TxStackPtr, bool>> mEvictionCache;

    std::vector<std::pair<int64, uint32_t>> mLaneEvictedFeeBid;

    std::shared_ptr<SurgePricingLaneConfig> mSurgePricingLaneConfig;

    uint32_t maxQueueSizeOps() const;

    void resetTxs();

  public:
    TxQueueLimiter(uint32 multiplier, Application& app);
    ~TxQueueLimiter();

    void addTransaction(TransactionFrameBasePtr const& tx);
    void removeTransaction(TransactionFrameBasePtr const& tx);
#ifdef BUILD_TESTS
    size_t size() const;
#endif
    // evict the worst transactions until there
    // is enough capacity to insert ops operations
    // by calling `evict` - note that evict must call `removeTransaction`
    // as to make space
    void evictTransactions(
        TransactionFrameBaseConstPtr newTx,
        std::function<void(TransactionFrameBasePtr const&)> evict);

    // oldTx is set when performing a replace by fee
    // return
    // first=true if transaction can be added
    // otherwise:
    //    second=0 if caller needs to wait
    //    second=minimum fee needed for tx to pass the next round of
    //    validation
    std::pair<bool, int64> canAddTx(TransactionFrameBasePtr const& tx,
                                    TransactionFrameBasePtr const& oldTx);

    void reset();

    void resetEvictionState();
};

bool lessThanXored(TransactionFrameBasePtr const& l,
                   TransactionFrameBasePtr const& r, size_t seed);
}
