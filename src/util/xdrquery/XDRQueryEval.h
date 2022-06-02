// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace xdrquery
{
using ResultValueType =
    std::variant<bool, int32_t, uint32_t, int64_t, uint64_t, std::string>;
using ResultType = std::optional<ResultValueType>;

using FieldResolver =
    std::function<ResultType(std::vector<std::string> const&)>;

char const NULL_LITERAL[] = "__NULL__";

enum class EvalNodeType
{
    LITERAL,
    FIELD,
    BOOL_OP,
    COMPARISON_OP
};

struct EvalNode
{
    virtual ResultType eval(FieldResolver const& fieldResolver) const = 0;
    virtual EvalNodeType getType() const = 0;

    virtual ~EvalNode() = default;
};

enum class LiteralNodeType
{
    INT,
    STR,
    NULL_LITERAL
};

struct LiteralNode : public EvalNode
{
    LiteralNode(LiteralNodeType valueType, std::string const& val);

    ResultType eval(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

    void resolveIntType(ResultValueType const& fieldValue,
                        std::vector<std::string> const& fieldPath) const;

    LiteralNodeType mType;
    mutable ResultType mValue;
};

struct FieldNode : public EvalNode
{
    FieldNode(std::string const& initField);

    ResultType eval(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

    std::vector<std::string> mFieldPath;
};

struct BoolEvalNode : public EvalNode
{
    ResultType eval(FieldResolver const& fieldResolver) const override;

    virtual bool evalBool(FieldResolver const& fieldResolver) const = 0;
};

enum class BoolOpNodeType
{
    AND,
    OR
};

struct BoolOpNode : public BoolEvalNode
{
    BoolOpNode(BoolOpNodeType nodeType, std::unique_ptr<BoolEvalNode> left,
               std::unique_ptr<BoolEvalNode> right);

    bool evalBool(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

  private:
    BoolOpNodeType mType;
    std::unique_ptr<BoolEvalNode> mLeft;
    std::unique_ptr<BoolEvalNode> mRight;
};

enum class ComparisonNodeType
{
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE
};

struct ComparisonNode : public BoolEvalNode
{
    ComparisonNode(ComparisonNodeType nodeType, std::unique_ptr<EvalNode> left,
                   std::unique_ptr<EvalNode> right);

    bool evalBool(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

  private:
    ComparisonNodeType mType;
    std::unique_ptr<EvalNode> mLeft;
    std::unique_ptr<EvalNode> mRight;
};
} // namespace xdrquery