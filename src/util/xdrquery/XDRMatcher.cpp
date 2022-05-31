// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/xdrquery/XDRMatcher.h"

#include "util/xdrquery/XDRQueryParser.h"

#include <sstream>
#include <stack>
#include <unordered_map>

namespace xdrquery
{
// namespace
//{

// template <typename T> class QueryEvaluator
//{
//  public:
//    QueryEvaluator(std::string const& query) : mQuery(query)
//    {
//    }
//
//    ResultType
//    evaluateQuery(T const& xdrMessage)
//    {
//        // Lazily evaluate the query in order to simplify exception handling.
//        // The query might fail both during the parsing and during matching
//        // process with XDRQueryError.
//        if (!mEvalRoot)
//        {
//            parseQuery();
//        }
//    }
//
//  private:
//
//    void
//    parseQuery()
//    {
//        std::stack<EvalNode*> nodes;
//        std::string curr_token;
//
//        for (char c : mQuery)
//        {
//            if (std::isspace(c))
//            {
//                continue;
//            }
//
//        }
//
//    }
//
//    std::string mQuery;
//
//    std::unique_ptr<EvalNode> mEvalRoot;
//    std::vector<std::pair<ValueNode*, std::vector<std::string>> mBoundValues;
//};

//}

XDRMatcher::XDRMatcher(std::string const& query)
{
    /*std::istringstream iss(query);
    scanner.switch_streams(iss, nullptr);*/
    /*yyscan_t scanner;
    yylex_init(&scanner);*/
    
    // yyparse(scanner);

    // setInputStr(query.c_str());
    //XDRQueryParser parser(mEvalRoot);
    //mEvalRoot = parseXDRQuery(query);
    //parser.parse();
     
    //yylex_destroy(scanner);
    // endScan();
}

}
