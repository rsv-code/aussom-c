%{
/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Ported from src/main/jflex/Scanner.jflex in aussom-base. Every
 * YYINITIAL rule, every start condition, and every escape-sequence
 * handler maps 1:1 to the Java source so dumped tokens line up
 * across the two implementations.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av_string.h"
#include "aussom/engine.h"
#include "aussom/parser_ctx.h"

/* The collector's gc.h pulls in windows.h on Windows, and windows.h
 * defines CALLBACK as a calling convention. The grammar has a token of
 * that name, so the generated token enum below would expand to a
 * storage specifier and fail to parse. CALLBACK is the only one of the
 * grammar's 74 tokens that collides, and neither the scanner nor the
 * parser uses the Windows macro. */
#if defined(_WIN32) && defined(CALLBACK)
#  undef CALLBACK
#endif

#include "parser.h"

/* Growing buffer used to accumulate the contents of string and doc
 * literals across multiple flex rules in a start condition. Lives
 * in GC memory so the final aussom_strdup is cheap. */
typedef struct strbuf {
    char *data;
    size_t len;
    size_t cap;
} strbuf;

/* Everything the scanner carries across rules, one per scanner rather
 * than one per process. This is what makes the lexer re-entrant: two
 * threads parsing at once each hold their own yyscan_t, and every
 * piece of state below reaches them through it.
 *
 * `column` is hand-tracked because flex counts lines (yylineno) but
 * leaves columns to the user. */
typedef struct lex_extra {
    parser_ctx *ctx;
    int column;
    strbuf sbuf;
} lex_extra;

/* The rules and the helpers below are written against the old global
 * names; these route them to the current scanner's own state. Valid
 * anywhere `yyscanner` is in scope, which is every rule action and
 * every helper that takes it. */
#define current_ctx (yyextra->ctx)
#define yycol       (yyextra->column)
#define string_buf  (yyextra->sbuf)

static void strbuf_reset(strbuf *b)
{
    b->len = 0;
    if (b->data != NULL) b->data[0] = '\0';
}

static void strbuf_reserve(strbuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t nc = (b->cap == 0) ? 64 : b->cap;
    while (nc < need) nc *= 2;
    b->data = AUSSOM_REALLOC(b->data, nc);
    b->cap = nc;
}

static void strbuf_append(strbuf *b, const char *s, size_t n)
{
    strbuf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void strbuf_append_char(strbuf *b, char c)
{
    strbuf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

/* UTF-8 encodes the given codepoint into the buffer. The Java side
 * appends the codepoint as a single Java char; doing the same in C
 * requires choosing an encoding. UTF-8 matches the way string
 * literals are read on disk. */
/* A \uXXXX escape. U+0000 is written as C0 80 and a surrogate as its
 * own 3-byte form, the lossless char form strings use (see
 * av_string.h), so neither ends the literal or is lost. */
static void strbuf_append_codepoint(strbuf *b, uint32_t cp)
{
    if (cp == 0) {
        strbuf_append(b, "\xC0\x80", 2);
    } else if (cp < 0x80u) {
        strbuf_append_char(b, (char)cp);
    } else if (cp < 0x800u) {
        char c[2];
        c[0] = (char)(0xC0u | (cp >> 6));
        c[1] = (char)(0x80u | (cp & 0x3Fu));
        strbuf_append(b, c, 2);
    } else if (cp < 0x10000u) {
        char c[3];
        c[0] = (char)(0xE0u | (cp >> 12));
        c[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        c[2] = (char)(0x80u | (cp & 0x3Fu));
        strbuf_append(b, c, 3);
    } else {
        char c[4];
        c[0] = (char)(0xF0u | (cp >> 18));
        c[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        c[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        c[3] = (char)(0x80u | (cp & 0x3Fu));
        strbuf_append(b, c, 4);
    }
}

/* The finished literal. Re-encoding it through UTF-16 units joins a
 * surrogate pair written as two escapes ("\ud83d\ude00") into one
 * character, so the literal's text matches the same string built at
 * run time. */
static char *strbuf_literal(const strbuf *b)
{
    const char *t = b->data ? b->data : "";
    size_t n;
    uint16_t *u = av_text_to_units(t, b->data ? b->len : 0, &n);
    return av_units_to_text(u, n, NULL);
}

/* Defined in the epilogue: flex emits `struct yyguts_t` -- which holds
 * yylineno and yyextra in re-entrant mode -- after this prologue, so a
 * helper that reaches into the scanner cannot live up here. */
static void lex_error(yyscan_t yyscanner, const char *msg, int col);
static void lex_illegal(yyscan_t yyscanner, const char *text, size_t len, int col);

/* The number of UTF-16 units in a matched UTF-8 token. Columns count
 * units, as Java's lexer counts chars: a continuation byte adds none and
 * a four-byte sequence adds two. */
static int utf16_len(const char *s, size_t n)
{
    int u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) != 0x80) u += (b >= 0xF0) ? 2 : 1;
    }
    return u;
}

/* Populate yylloc and advance the column for every matched token.
 * yylineno is auto-incremented by flex when the rule body contains
 * a newline; we still reset the column explicitly on '\n' rules so
 * column tracking stays accurate across all start conditions. */
#define YY_USER_ACTION                                  \
    do {                                                \
        yylloc->first_line = yylineno;                  \
        yylloc->first_column = yycol;                   \
        yylloc->last_line = yylineno;                   \
        int units_ = utf16_len(yytext, yyleng);         \
        yylloc->last_column = yycol + units_ - 1;       \
        yycol += units_;                                \
    } while (0);

%}

%option noyywrap
%option yylineno
/* Re-entrant: all scanner state moves into a heap yyscan_t rather than
 * file-scope globals, so two threads can parse at once. bison-bridge
 * passes yylval, bison-locations passes yylloc, both as pointers. */
%option reentrant bison-bridge bison-locations
%option extra-type="lex_extra *"
%option nounput
%option noinput
/* A build error, rather than flex's default rule, when some input in
 * some state matches no rule. The default rule copies the text to
 * yyout, which is the process's stdout, not the engine's logger. */
%option nodefault

%x AUSSOM_DOC
%x C_COMMENT
%x STR
%x STR_FRMT
%x STR_LIT

WHITE_SPACE     [ \t\f]
EXPONENT        [eE][+\-]?[0-9]+
NUMBER          [0-9]+("."[0-9]+)?{EXPONENT}?
ALPHANUMERIC    [A-Za-z0-9]
IDENTIFIER      ({ALPHANUMERIC}|_)*

%%

    /* Single-line comment. */
"//"[^\r\n]*                        { /* ignore */ }

    /* Keywords. Listed before IDENTIFIER so they take precedence
     * via flex's longest-match-then-rule-order resolution. */
"class"                             { return CLASSDEF; }
"enum"                              { return ENUM; }
"include"                           { return INCLUDE; }
"public"                            { return PUBLIC; }
"private"                           { return PRIVATE; }
"static"                            { return STATIC; }
"extern"                            { return EXTERN; }
"return"                            { return RETURN; }
"try"                               { return TRY; }
"catch"                             { return CATCH; }
"throw"                             { return THROW; }
"new"                               { return NEW; }
"null"                              { return KW_NULL; }
"if"                                { return IF; }
"else"                              { return ELSE; }
"switch"                            { return SWITCH; }
"case"                              { return CASE; }
"default"                           { return DEFAULT; }
"while"                             { return WHILE; }
"break"                             { return BREAK; }
"for"                               { return FOR; }
"instanceof"                        { return INSTANCEOF; }
":="                                { return SET; }

";"                                 { return SEMI; }
":"                                 { return COLON; }
","                                 { return COMMA; }
"("                                 { return LPAREN; }
")"                                 { return RPAREN; }
"{"                                 { return LBRACKET; }
"}"                                 { return RBRACKET; }
"["                                 { return LBRACE; }
"]"                                 { return RBRACE; }

"="                                 { return EQ; }
"+="                                { return PLEQ; }
"-="                                { return MIEQ; }
"*="                                { return MUEQ; }
"/="                                { return DIEQ; }
"%="                                { return MODULUSEQ; }
"++"                                { return PLPL; }
"--"                                { return MIMI; }
"+"                                 { return PL; }
"-"                                 { return MI; }
"*"                                 { return MU; }
"~/"                                { return FLOORDIV; }
"/"                                 { return DI; }
"%"                                 { return MODULUS; }
"?"                                 { return MISSNULL; }

"=="                                { return EQEQ; }
"!="                                { return NOTEQ; }
"<"                                 { return LT; }
">"                                 { return GT; }
"<="                                { return LTEQ; }
">="                                { return GTEQ; }
"!"                                 { return NOT; }
"&&"                                { return AND; }
"||"                                { return OR; }
"&"                                 { return ANDB; }
"|"                                 { return ORB; }
"true"                              { yylval->bool_val = true;  return LIT_BOOL; }
"false"                             { yylval->bool_val = false; return LIT_BOOL; }
"..."                               { return ETCETERA; }
"::"                                { return CALLBACK; }
"#"                                 { return COUNT; }
"@="                                { return INSERT; }
"@"                                 { return AT; }

"."                                 { return DOT; }

    /* Start-condition openers. The doc-comment opener uses
     * trailing context (the slash separator) so an empty block
     * comment does not begin a doc block. */
"/**"/[^*/]                         { strbuf_reset(&string_buf); BEGIN(AUSSOM_DOC); }
"/*"                                { BEGIN(C_COMMENT); }
\"\"\"                              { strbuf_reset(&string_buf); BEGIN(STR_FRMT); }
\"                                  { strbuf_reset(&string_buf); BEGIN(STR); }
\'                                  { strbuf_reset(&string_buf); BEGIN(STR_LIT); }

    /* Whitespace and line endings. The newline rule also resets
     * yycolumn so the next line starts at column 1. yylineno is
     * incremented by flex automatically. */
\n                                  { yycol = 1; }
\r\n                                { yycol = 1; }
{WHITE_SPACE}+                      { /* ignore */ }

    /* Number literal. Distinguishes int vs double by the presence
     * of '.', 'e', or 'E'. The text goes to the grammar, which checks
     * the range and converts it: aussom-base's lexer reads any length
     * of number (BigInteger, BigDecimal) and its grammar throws when
     * the value does not fit, quoting the value. */
{NUMBER}                            {
                                        bool is_double = false;
                                        for (size_t i = 0; yytext[i] != '\0'; i++) {
                                            char c = yytext[i];
                                            if (c == '.' || c == 'e' || c == 'E') { is_double = true; break; }
                                        }
                                        yylval->str_val = AUSSOM_STRDUP(yytext);
                                        return is_double ? LIT_DOUBLE : LIT_INT;
                                    }

    /* Identifier. Matches keywords too, but the keyword rules
     * above bind first via longest-match-then-rule-order. */
{IDENTIFIER}                        {
                                        yylval->str_val = AUSSOM_STRDUP(yytext);
                                        return IDENT;
                                    }

    /* ----- triple-quoted format string ----- */
<STR_FRMT>{
    \"\"\"                          {
                                        BEGIN(INITIAL);
                                        yylval->str_val = strbuf_literal(&string_buf);
                                        return LIT_STRING;
                                    }
    \\\"\\\"\\\"                    { strbuf_append(&string_buf, "\"\"\"", 3); }
    \t                              { strbuf_append_char(&string_buf, '\t'); }
    \n                              { yycol = 1; strbuf_append_char(&string_buf, '\n'); }
    \r                              { strbuf_append_char(&string_buf, '\r'); }
    \\\\                            { strbuf_append_char(&string_buf, '\\'); }
    \\u[0-9a-fA-F]{4}               {
                                        uint32_t cp = (uint32_t)strtoul(yytext + 2, NULL, 16);
                                        strbuf_append_codepoint(&string_buf, cp);
                                    }
    .                               { strbuf_append(&string_buf, yytext, (size_t)yyleng); }
}

    /* ----- double-quoted string ----- */
<STR>{
    \"                              {
                                        BEGIN(INITIAL);
                                        yylval->str_val = strbuf_literal(&string_buf);
                                        return LIT_STRING;
                                    }
    [^\n\r\"\\]+                    { strbuf_append(&string_buf, yytext, (size_t)yyleng); }
    [\t\r\n]+                       { /* ignore */ }
    \\t                             { strbuf_append_char(&string_buf, '\t'); }
    \\n                             { strbuf_append_char(&string_buf, '\n'); }
    \\r                             { strbuf_append_char(&string_buf, '\r'); }
    \\\"                            { strbuf_append_char(&string_buf, '"'); }
    \\\\                            { strbuf_append_char(&string_buf, '\\'); }
    \\u[0-9a-fA-F]{4}               {
                                        uint32_t cp = (uint32_t)strtoul(yytext + 2, NULL, 16);
                                        strbuf_append_codepoint(&string_buf, cp);
                                    }
    /* A backslash no rule above takes (an unknown escape). Java's
     * scanner reaches its error fallback here; the character after it
     * is then read as ordinary text. */
    \\                              { lex_illegal(yyscanner, yytext, yyleng, yylloc->first_column); }
}

    /* ----- single-quoted string literal ----- */
<STR_LIT>{
    \'                              {
                                        BEGIN(INITIAL);
                                        yylval->str_val = strbuf_literal(&string_buf);
                                        return LIT_STRING;
                                    }
    [^\n\r\'\\]+                    { strbuf_append(&string_buf, yytext, (size_t)yyleng); }
    [\t\r\n]+                       { /* ignore */ }
    \\t                             { strbuf_append_char(&string_buf, '\t'); }
    \\n                             { strbuf_append_char(&string_buf, '\n'); }
    \\r                             { strbuf_append_char(&string_buf, '\r'); }
    \\\'                            { strbuf_append_char(&string_buf, '\''); }
    \\\\                            { strbuf_append_char(&string_buf, '\\'); }
    \\u[0-9a-fA-F]{4}               {
                                        uint32_t cp = (uint32_t)strtoul(yytext + 2, NULL, 16);
                                        strbuf_append_codepoint(&string_buf, cp);
                                    }
    /* Unknown escape; see <STR>. */
    \\                              { lex_illegal(yyscanner, yytext, yyleng, yylloc->first_column); }
}

    /* ----- doc comment ----- */
<AUSSOM_DOC>{
    [^*\n]*                         { strbuf_append(&string_buf, yytext, (size_t)yyleng); }
    "*"+[^*/\n]*                    { strbuf_append(&string_buf, yytext, (size_t)yyleng); }
    \n                              { yycol = 1; strbuf_append_char(&string_buf, '\n'); }
    "*"+"/"                         {
                                        BEGIN(INITIAL);
                                        yylval->str_val = strbuf_literal(&string_buf);
                                        return AUSSOM_DOC_TOK;
                                    }
}

    /* ----- C-style block comment ----- */
<C_COMMENT>{
    [^*\n]*                         { /* ignore */ }
    "*"+[^*/\n]*                    { /* ignore */ }
    \n                              { yycol = 1; }
    "*"+"/"                         { BEGIN(INITIAL); }
}

    /* Error fallback: one character, which may be several bytes. */
[\xC2-\xF4][\x80-\xBF]{1,3}        { lex_illegal(yyscanner, yytext, yyleng, yylloc->first_column); }
.                                   { lex_illegal(yyscanner, yytext, yyleng, yylloc->first_column); }

%%

/* Java's "Illegal character <n>", once per UTF-16 unit: n is the unit,
 * so a character past U+FFFF gives two errors, one column apart. Bytes
 * that are not valid UTF-8 read as U+FFFD, as Java's decoder reads
 * them. */
static void lex_illegal(yyscan_t yyscanner, const char *text, size_t len, int col)
{
    size_t n;
    uint16_t *u = av_text_to_units(text, len, &n);
    bool valid = (unsigned char)text[0] < 0x80 || n == 1 ||
                 (n == 2 && u[0] >= 0xD800 && u[0] < 0xDC00);
    for (size_t i = 0; i < (valid ? n : 1); i++) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Illegal character <%u>", valid ? (unsigned)u[i] : 0xFFFDu);
        lex_error(yyscanner, buf, col + (int)i);
    }
}

/* Reports a lexer error the way Java's Lexer.error does, at the 1-based
 * column where the bad text starts. */
static void lex_error(yyscan_t yyscanner, const char *msg, int col)
{
    /* yylineno, yyextra and friends are members of the scanner struct
     * in re-entrant mode, reached through `yyg`. flex declares it for
     * yylex itself; a helper has to unpack the handle. */
    struct yyguts_t *yyg = (struct yyguts_t *)yyscanner;

    if (current_ctx != NULL) {
        current_ctx->syntax_errors = true;
        engine_set_parse_error(current_ctx->eng);
        engine_add_parse_diagnostic(current_ctx->eng, current_ctx->file_name,
                                    yylineno, col, msg);
        /* Through the owning engine's logger, so a syntax error is
         * attributable to the engine that was parsing and a host
         * capturing one engine's output does not receive another's. */
        char text[640];
        snprintf(text, sizeof(text), "%s [%d]: Error at line %d, column %d : %s",
            current_ctx->file_name, yylineno, yylineno, col, msg);
        engine_err(current_ctx->eng, text);
    } else {
        /* Driven standalone, with no engine to attribute the message
         * to; stderr is the only sensible destination. */
        fprintf(stderr, "[%d]: Error at line %d, column %d : %s\n",
            yylineno, yylineno, col, msg);
    }
}

/* Creates a scanner reading `fp`, owning its own line/column tracking
 * and string buffer. Defined here in the epilogue where BEGIN /
 * INITIAL are in scope. Returns NULL if the scanner cannot be built.
 *
 * Line and column start at 1:1 for every scanner, which is what used
 * to need an explicit reset: yyrestart swapped the input buffer but
 * left yylineno alone, so each parse inherited the previous one's line
 * count. A fresh scanner has no previous parse to inherit from. */
void *aussom_lex_begin(parser_ctx *ctx, FILE *fp)
{
    yyscan_t scanner;
    lex_extra *extra = AUSSOM_ALLOC(sizeof(*extra));
    extra->ctx = ctx;
    extra->column = 1;
    extra->sbuf.data = NULL;
    extra->sbuf.len = 0;
    extra->sbuf.cap = 0;

    /* Before yylex_init_extra, so that the only window in which the
     * extra is unreachable is the two statements above, where it is
     * still live in a local. After this the parser_ctx holds it and the
     * collector can see it; without it the sole remaining pointer lives
     * inside flex's malloc'd scanner, which the collector does not
     * trace, and a collection during the parse frees the scanner's
     * state out from under it. */
    ctx->lex_extra = extra;

    if (yylex_init_extra(extra, &scanner) != 0) return NULL;
    yyset_in(fp, scanner);
    /* No yyset_lineno here: yylex_init already starts the counter at 1,
     * and yyset_lineno needs an input buffer, which flex does not
     * create until the first yylex call. */
    {
        struct yyguts_t *yyg = (struct yyguts_t *)scanner;
        BEGIN(INITIAL);
    }
    return scanner;
}

/* Releases a scanner built by aussom_lex_begin. The lex_extra and its
 * buffer are GC memory and need no explicit free. */
void aussom_lex_end(void *scanner)
{
    if (scanner != NULL) yylex_destroy((yyscan_t)scanner);
}
