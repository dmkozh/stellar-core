// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/xdrquery/XDRQueryEval.h"
#include "util/xdrquery/XDRMatcher.hpp"
#include <string>
#include <variant>

namespace xdrquery
{

class XDRMatcher
{
  public:
    XDRMatcher(std::string const& query);

    template <typename T> bool matchXDR(T const& xdrMessage)
    {
        return mEvalRoot->evalBool(
            [&xdrMessage](std::vector<std::string> const& fieldPath)
            {
                XDRFieldResolver resolver(fieldPath);
                xdr::xdr_argpack_archive(resolver, xdrMessage);
                return resolver.getResult();
            });
        

    }

  private:
    std::shared_ptr<BoolEvalNode> mEvalRoot;
};
}

#include "util/xdrquery/XDRMatcher.hpp"