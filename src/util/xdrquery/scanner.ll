%{
#include "util/xdrquery/parser.hh"
#include "util/xdrquery/XDRQueryError.h"
#include "util/xdrquery/XDRQueryEval.h"
%}

%option noyywrap
%option nounput noinput

IDENTIFIER  [a-zA-Z_][a-zA-Z_0-9]
INT -?[0-9]+
STRING \".*\"
WHITESPACE [ \t\r\n]

%%

{WHITESPACE}+ /* discard */;

{IDENTIFIER}  { yylval.str = std::string(yytext); return T_ID; }
{INT}         { yylval.str = std::string(yytext); return T_INT; }

{STRING} { 
    yylval.str = std::string(yytext + 1); 
    yylval.str.pop_back();
    return T_STR;
}

"&&"  { return T_AND; }
"||"  { return T_OR; }

"=="  { return T_EQ; }
"!="  { return T_NE; }
">="  { return T_GE; }
">"   { return T_GT; }
"<="  { return T_LE; }
"<"   { return T_LT; }

"("   { return T_LPAREN; }
")"   { return T_RPAREN; }

"."   { return T_DOT; }

. { throw xdrquery::XDRQueryError("Unexpected character: " + std::string(yytext)); }

%%

void setInput(std::string const& str) {
  yy_scan_string(str.c_str());
}

void endScan() {
  yy_delete_buffer(YY_CURRENT_BUFFER);
}
