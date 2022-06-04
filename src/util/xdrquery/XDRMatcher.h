// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/xdrquery/XDRFieldResolver.h"
#include "util/xdrquery/XDRQueryEval.h"
#include <string>
#include <variant>

namespace xdrquery
{

class XDRMatcher
{
  public:
    XDRMatcher(std::string const& query);

    template <typename T>
    bool
    matchXDR(T const& xdrMessage)
    {
        return matchInternal(
            [&xdrMessage, this](std::vector<std::string> const& fieldPath) {
                if (mFirstMatch)
                {
                    mFirstMatch = false;
                    return getXDRFieldValidated(xdrMessage, fieldPath);
                }
                return getXDRField(xdrMessage, fieldPath);
            });
    }

  private:
    bool matchInternal(FieldResolver const& fieldResolver);

    std::string const mQuery;
    std::unique_ptr<BoolEvalNode> mEvalRoot;
    bool mFirstMatch = true;
};
} // namespace xdrquery
