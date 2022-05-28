// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <string>
#include <variant>

namespace xdrquery
{

class XDRMatcher
{
  public:
    XDRMatcher(std::string const& query);

    template <typename T> bool matchXdr(T const& xdrMessage);

  private:
};
}

#include "util/xdrquery/XDRMatcher.hpp"