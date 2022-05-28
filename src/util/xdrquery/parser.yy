%{
#include "fmt/format.h"
#include "util/xdrquery/XDRQueryError.h"
#include "util/xdrquery/XDRQueryEval.h"

#include <memory>

using namespace xdrquery;
%}

%union {
  std::string str;
  std::unique_ptr<EvalNode> node;
  std::unique_ptr<FieldNode> field_node;
  std::unique_ptr<BoolEvalNode> bool_node;
}

%token <str> T_ID
%token <str> T_INT
%token <str> T_STR

%token T_AND "&&"
%token T_OR "||"

%token T_EQ "=="
%token T_NE "!="
%token T_GT ">"
%token T_GE ">="
%token T_LT "<"
%token T_LE "<="

%token T_LPAREN "("
%token T_RPAREN ")"

%token T_DOT "."

%left "||"
%left "&&"
%left "==" "!=" ">" ">=" "<" "<="

%type <node> literal operand
%type <bool_node> comparison_expr logic_expr
%type <field_node> field

%%

logic_expr: comparison_expr { $$ = std::move($1); }
          | comparison_expr "&&" logic_expr {
            $$ = std::make_unique<BoolOpNode>(BoolOpNodeType::AND,
                std::move($1), std::move($3)); }
          | comparison_expr "||" logic_expr {
            $$ = std::make_unique<BoolOpNode>(BoolOpNodeType::OR,
                std::move($1), std::move($3)); }
          | "(" logic_expr ")" { $$ = std::move($2); }

comparison_expr: operand "==" operand {
        $$ = std::make_unique<ComparisonNode>(ComparisonNodeType::EQ,
                std::move($1), std::move($3)); }
    | operand "!=" operand {
        $$ = std::make_unique<ComparisonNode>(ComparisonNodeType::NE,
                std::move($1), std::move($3)); }
    | operand "<" operand {
        $$ = std::make_unique<ComparisonNode>(ComparisonNodeType::LT,
                std::move($1), std::move($3)); }
    | operand "<=" operand {
        $$ = std::make_unique<ComparisonNode>(ComparisonNodeType::LE,
                std::move($1), std::move($3)); }
    | operand ">" operand {
        $$ = std::make_unique<ComparisonNode>(ComparisonNodeType::GT,
                std::move($1), std::move($3)); }
    | operand ">=" operand {
        $$ = std::make_unique<ComparisonNode>(ComparisonNodeType::GE,
                std::move($1), std::move($3)); }

operand: literal { $$ = std::move($1); }
       | field { $$ = std::move($1); }

literal: T_INT { $$ = std::make_unique<LiteralNode>(LiteralNodeType::INT, $1); }
       | T_STR { $$ = std::make_unique<LiteralNode>(LiteralNodeType::STR, $1); }

field: T_ID { $$ = std::make_unique<FieldNode>($1); }
     | field "." T_ID { $1->mFieldPath.push_back($3); }

%%

void yy::parser::error(location_type const& loc, std::string const& msg) {
  throw XDRQueryError(
    fmt::format(FMT_STRING("Query parse error at {}: {}"), loc, msg);
}

void setInput(std::string const& str);
void endScan();

std::unique_ptr<EvalNode> parseQuery(std::string const& query) {
  setInput(query);
  auto res = yyparse();
  endScan();
  return res;
}
