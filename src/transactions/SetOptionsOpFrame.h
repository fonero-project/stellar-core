#pragma once

// Copyright 2015 Fonero Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace fonero
{
class SetOptionsOpFrame : public OperationFrame
{
    ThresholdLevel getThresholdLevel() const override;
    SetOptionsResult&
    innerResult()
    {
        return mResult.tr().setOptionsResult();
    }
    SetOptionsOp const& mSetOptions;

  public:
    SetOptionsOpFrame(Operation const& op, OperationResult& res,
                      TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static SetOptionsResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().setOptionsResult().code();
    }
};
}
