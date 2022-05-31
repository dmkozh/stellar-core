%{
#ifdef _MSC_VER
#include <io.h>
#define popen _popen
#define pclose _pclose
#define access _access
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

#include "util/xdrquery/XDRQueryParser.h"
%}

%option noyywrap
%option nounput noinput
%option batch

IDENTIFIER  [a-zA-Z_][a-zA-Z_0-9]
INT -?[0-9]+
STRING \".*\"
WHITESPACE [ \t\r\n]

%%

{WHITESPACE}+ /* discard */;

{IDENTIFIER}  { return xdrquery::XDRQueryParser::make_ID(yytext); }
{INT}         { return xdrquery::XDRQueryParser::make_INT(yytext); }

{STRING} { 
    std::string s(yytext + 1); 
    s.pop_back();
    return xdrquery::XDRQueryParser::make_STR(s);
}

"&&"  { return xdrquery::XDRQueryParser::make_AND(); }
"||"  { return xdrquery::XDRQueryParser::make_OR(); }

"=="  { return xdrquery::XDRQueryParser::make_EQ(); }
"!="  { return xdrquery::XDRQueryParser::make_NE(); }
">="  { return xdrquery::XDRQueryParser::make_GE(); }
">"   { return xdrquery::XDRQueryParser::make_GT(); }
"<="  { return xdrquery::XDRQueryParser::make_LE(); }
"<"   { return xdrquery::XDRQueryParser::make_LT(); }

"("   { return xdrquery::XDRQueryParser::make_LPAREN(); }
")"   { return xdrquery::XDRQueryParser::make_RPAREN(); }

"."   { return xdrquery::XDRQueryParser::make_DOT(); }

.     { throw xdrquery::XDRQueryParser::syntax_error("Unexpected character: " + std::string(yytext)); }

%%

void setInputStr(char const* s) {
  yy_scan_string(s);
}

void endScan() {
  yy_delete_buffer(YY_CURRENT_BUFFER);
}
