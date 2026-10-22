/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Ported from src/main/cup/aussom.cup in aussom-base. Every CUP
 * production maps 1:1 to a bison rule so the resulting AST is the
 * same shape as the Java side.
 *
 * Locations: CUP exposes per-symbol Xleft and Xright which the Java
 * action calls plumb into setParserInfo. Bison's @N gives the full
 * location range. The mapping used throughout this file:
 *
 *     CUP                       bison
 *     ---                       -----
 *     Xleft  (start line)       @N.first_line
 *     Xright (start column)     @N.first_column
 *     cur_token.left  (end ln)  @$.last_line
 *     cur_token.right (end col) @$.last_column
 */

/* Emitted first in the generated .c, before everything else. */
%code top {
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

/* Emitted into the generated parser.h before the YYSTYPE union, so
 * every consumer of parser.h (notably scanner.l) sees the AST types
 * and the grammar-local string_array the union refers to. Also
 * re-emitted near the top of the .c file, ahead of the %code block
 * below. */
%code requires {
#include "aussom/ast.h"
#include "aussom/parser_ctx.h"

typedef struct string_array {
    char **items;
    size_t count;
    size_t capacity;
} string_array;
}

/* Emitted in the .c file after %code requires, so the AST types and
 * string_array are already visible here. */
%code {
#include "aussom/aussom_gc.h"
#include "aussom/security_manager.h"
#include "aussom/memstream.h"

/* Forward-declares from scanner.l. */
/* flex's re-entrant scanner with bison-bridge and bison-locations:
 * the token value and location come back through pointers instead of
 * globals, and the scanner handle carries the rest. */
extern int yylex(YYSTYPE *yylval_param, YYLTYPE *yylloc_param, void *scanner);
extern int yylineno;
extern FILE *yyin;
void yyrestart(FILE *input_file);
/* Scanner lifecycle, defined in scanner.l. The handle is void * here
 * so the grammar does not need flex's yyscan_t in scope. */
void *aussom_lex_begin(parser_ctx *ctx, FILE *fp);
void aussom_lex_end(void *scanner);

static void yyerror(YYLTYPE *yyllocp, void *scanner, parser_ctx *ctx,
                    const char *fmt, ...);

/* Helper: stamp file_name + start/end positions on an AST node. */
#define SET_INFO_R(node, sl, sc, el, ec)                              \
    ast_node_set_parser_info_range((ast_node *)(node),                \
        ctx->file_name, (sl), (sc), (el), (ec))

static string_array *string_array_new(void)
{
    string_array *a = AUSSOM_ALLOC(sizeof(*a));
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
    return a;
}

static void string_array_add(string_array *a, const char *s)
{
    if (a->count == a->capacity) {
        size_t nc = (a->capacity == 0) ? 4 : a->capacity * 2;
        a->items = AUSSOM_REALLOC(a->items, nc * sizeof(*a->items));
        a->capacity = nc;
    }
    a->items[a->count++] = AUSSOM_STRDUP(s);
}

static ast_node_array *node_array_new(void)
{
    ast_node_array *a = AUSSOM_ALLOC(sizeof(*a));
    ast_node_array_init(a);
    return a;
}

static ast_node_pair_array *pair_array_new(void)
{
    ast_node_pair_array *a = AUSSOM_ALLOC(sizeof(*a));
    ast_node_pair_array_init(a);
    return a;
}

/* One parse-error console report, routed to the owning engine's logger
 * so the message is attributable to the engine that was parsing and a
 * host capturing one engine's output never receives another's. `eng`
 * is NULL only when the parser is driven with no engine in scope, in
 * which case stderr is the only destination left.
 *
 * Always paired with an engine_add_parse_diagnostic at the call site:
 * the structured record and the human message go out together, never
 * one instead of the other. */
static void parse_report(engine *eng, const char *file_name, int line,
                         const char *msg)
{
    char text[640];
    snprintf(text, sizeof(text), "%s [%d]: PARSE_ERROR: %s",
             file_name != NULL ? file_name : "?", line, msg);
    if (eng != NULL) engine_err(eng, text);
    else fprintf(stderr, "%s\n", text);
}

/* Stops the parse with an error aussom-base throws as
 * aussomException(String Message), whose message is the whole text.
 * As in Java, the error is not logged, recorded as a diagnostic or
 * flagged here: it goes to whoever started the parse, through
 * engine_parse_exception. The grammar action that led here checks
 * ctx->thrown and stops the parse with YYABORT (ABORT_IF_THROWN). */
static void parse_throw(parser_ctx *ctx, const char *msg)
{
    ctx->thrown = true;
    engine_set_parse_exception(ctx->eng, msg, msg);
}

/* The same for aussomException(astNode, Message, ""), whose
 * getMessage() puts the node's file and line in front of the message
 * and a newline after it. */
static void parse_throw_at(parser_ctx *ctx, const char *file_name, int line,
                           const char *msg)
{
    if (file_name == NULL) file_name = "";
    size_t n = strlen(file_name) + strlen(msg) + 32;
    char *full = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(full, n, "%s [%d]: %s\n", file_name, line, msg);
    ctx->thrown = true;
    engine_set_parse_exception(ctx->eng, full, msg);
}

/* Used in a grammar action right after anything that can throw. */
#define ABORT_IF_THROWN() do { if (ctx->thrown) YYABORT; } while (0)

/* --- number literal range checks ----------------------------------
 *
 * aussom-base's lexer reads an int literal as a BigInteger and a double
 * literal as a BigDecimal, and the grammar throws when the value does
 * not fit a long or a double, quoting the value as Java prints it. The
 * scanner hands the literal's text to the grammar so the same checks
 * and messages can be made here. */

/* An int literal's digits without leading zeros, as BigInteger prints
 * them. */
static const char *int_lit_digits(const char *t)
{
    while (t[0] == '0' && t[1] != '\0') t++;
    return t;
}

/* Compares two digit strings that have no leading zeros, as numbers. */
static int cmp_digits(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(a, b);
}

/* The value of an int literal in range: the magnitude is at most 2^63,
 * and the unsigned negate keeps -9223372036854775808 well-defined. */
static int64_t int_lit_value(const char *digits, bool neg)
{
    uint64_t uv = strtoull(digits, NULL, 10);
    return neg ? (int64_t)(-uv) : (int64_t)uv;
}

/* A double literal as BigDecimal holds it: the unscaled value's digits
 * (no leading zeros) and the scale, so the value is
 * digits * 10^-scale. */
typedef struct dec_lit {
    char *digits;
    long long scale;
} dec_lit;

static dec_lit dec_lit_parse(const char *t)
{
    dec_lit d;
    size_t n = strlen(t);
    char *digits = AUSSOM_ALLOC_ATOMIC(n + 1);
    size_t k = 0;
    long long frac = 0;
    bool in_frac = false;
    long long exp = 0;
    for (const char *c = t; *c != '\0'; c++) {
        if (*c == '.') { in_frac = true; continue; }
        if (*c == 'e' || *c == 'E') { exp = strtoll(c + 1, NULL, 10); break; }
        digits[k++] = *c;
        if (in_frac) frac++;
    }
    digits[k] = '\0';
    d.digits = (char *)int_lit_digits(digits);
    d.scale = frac - exp;
    return d;
}

/* BigDecimal.negate() of d times -1.0, as the grammar's `MI DOUBLE` rule
 * computes it: the unscaled value gains a trailing zero and the scale
 * goes up by one. */
static dec_lit dec_lit_times_neg_one(dec_lit d)
{
    if (strcmp(d.digits, "0") != 0) {
        size_t n = strlen(d.digits);
        char *x = AUSSOM_ALLOC_ATOMIC(n + 2);
        memcpy(x, d.digits, n);
        x[n] = '0';
        x[n + 1] = '\0';
        d.digits = x;
    }
    d.scale += 1;
    return d;
}

/* BigDecimal.toString(): plain notation when the scale is not negative
 * and the adjusted exponent is at least -6, scientific otherwise. */
static char *dec_lit_to_string(dec_lit d, bool neg)
{
    size_t len = strlen(d.digits);
    long long adjusted = -d.scale + (long long)(len - 1);
    size_t cap = len + 64 + (d.scale > 0 ? (size_t)d.scale : 0);
    char *out = AUSSOM_ALLOC_ATOMIC(cap);
    size_t k = 0;
    if (neg) out[k++] = '-';
    if (d.scale == 0) {
        memcpy(out + k, d.digits, len);
        k += len;
    } else if (d.scale > 0 && adjusted >= -6) {
        if ((long long)len > d.scale) {
            size_t ip = len - (size_t)d.scale;
            memcpy(out + k, d.digits, ip);
            k += ip;
            out[k++] = '.';
            memcpy(out + k, d.digits + ip, len - ip);
            k += len - ip;
        } else {
            out[k++] = '0';
            out[k++] = '.';
            for (long long i = 0; i < d.scale - (long long)len; i++) out[k++] = '0';
            memcpy(out + k, d.digits, len);
            k += len;
        }
    } else {
        out[k++] = d.digits[0];
        if (len > 1) {
            out[k++] = '.';
            memcpy(out + k, d.digits + 1, len - 1);
            k += len - 1;
        }
        if (adjusted != 0) {
            k += (size_t)snprintf(out + k, cap - k, "E%s%lld",
                                  adjusted > 0 ? "+" : "", adjusted);
        }
    }
    out[k] = '\0';
    return out;
}

/* Every digit of Double.MAX_VALUE, which is an integer. */
static const char DBL_MAX_DIGITS[] =
    "17976931348623157081452742373170435679807056752584499659891747680315"
    "72607800285387605895586327668781715404589535143824642343213268894641"
    "82768467546703537516986049910576551282076245490090389328944075868508"
    "45513394230458323690322294816580855933212334827479782620414472316873"
    "8177180919299881250404026184124858368";

/* True when d's magnitude is greater than Double.MAX_VALUE, compared
 * exactly, the way BigDecimal.compareTo does. */
static bool dec_lit_over_max(dec_lit d)
{
    if (strcmp(d.digits, "0") == 0) return false;
    size_t len = strlen(d.digits);
    long long adjusted = -d.scale + (long long)(len - 1);
    long long max_adjusted = (long long)sizeof(DBL_MAX_DIGITS) - 2;
    if (adjusted != max_adjusted) return adjusted > max_adjusted;
    /* Same magnitude: compare the significant digits left to right. */
    size_t mlen = sizeof(DBL_MAX_DIGITS) - 1;
    while (len > 1 && d.digits[len - 1] == '0') len--;
    for (size_t i = 0; i < len; i++) {
        char m = (i < mlen) ? DBL_MAX_DIGITS[i] : '0';
        if (d.digits[i] != m) return d.digits[i] > m;
    }
    return false;
}

/* Adds a function to the class, refusing a signature that collides with
 * one already declared. Two overloads that some call could reach
 * interchangeably are a program error, so this stops the parse with
 * astClass.duplicateSignatureError's message. Java builds that error on
 * the class node, which has no position yet while its body is being
 * read, hence the empty file and line 0 in front. */
static void add_function_checked(parser_ctx *ctx, ast_class *cls,
                                 ast_funct_def *fd)
{
    if (ctx->thrown) return;
    char *sig = NULL;
    ast_funct_def *prior = ast_class_add_function(cls, fd, &sig);
    if (prior != NULL) {
        size_t n = strlen(fd->base.name) + strlen(sig) + 512;
        char *msg = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(msg, n,
            "Duplicate method overload '%s(%s)' declared at %s:%d; first "
            "declared at %s:%d.",
            fd->base.name, sig,
            fd->base.file_name != NULL ? fd->base.file_name : "",
            fd->base.line_num,
            prior->base.file_name != NULL ? prior->base.file_name : "",
            prior->base.line_num);
        parse_throw_at(ctx, "", 0, msg);
    }
}

/* Builds the doc node for a doc comment, honoring the engine's
 * aussomdoc.retain policy.
 *
 * A node is attached either way, so a consumer sees the same shape
 * whatever policy says; only the text differs. Returning NULL instead
 * would save a little more but would break any caller that reasonably
 * reads the node without checking, and those callers are in other
 * projects where the breakage would surface as a crash.
 *
 * Defaults to true, because dropping the text is a silent loss for
 * every consumer: doc generation, reflect, and any tooling on top of
 * them. This is the one security property whose absent value is true;
 * it is parse behavior rather than an action gate, and an action gate
 * must deny when absent. */
static ast_aussom_doc *doc_node(parser_ctx *ctx, const char *text)
{
    engine *eng = ctx->eng;
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "aussomdoc.retain", true)) {
        return ast_aussom_doc_new_dropped();
    }
    return ast_aussom_doc_new_text(text);
}

/* Loads an include as soon as the parser reaches it, the way
 * Engine.addInclude runs from the CUP action. A refusal stops this
 * parse, as Java's throw does. So does an error that stopped the parse
 * of the included source: in Java that exception passes up through the
 * include and out of this parse too. */
static void load_include(parser_ctx *ctx, const char *name)
{
    const char *err = engine_add_include(ctx->eng, name);
    if (err != NULL) {
        parse_throw(ctx, err);
        return;
    }
    if (engine_parse_exception(ctx->eng) != NULL) ctx->thrown = true;
}

/* Binds an extern class after checking that this engine's policy
 * permits the named class. Ports parser.bindExternClass and
 * astClass.loadExternClass. The check is here rather than at
 * instantiation because the parser is what holds the engine, and this
 * is the single point where an extern class name enters it: a class
 * that cannot be declared cannot be constructed or called.
 *
 * A denied reference is never looked up. A denial and a missing class
 * both stop the parse, as Java's throw does, so the failures look
 * alike to a caller. */
static void bind_extern_class(parser_ctx *ctx, ast_class *cls,
                              char *extern_name)
{
    engine *eng = ctx->eng;
    char msg[384];
    if (!engine_is_extern_class_allowed(eng, extern_name)) {
        snprintf(msg, sizeof(msg),
            "Extern class '%s' is not permitted. Add it, or a matching "
            "'<package>.*' prefix, to the security manager property "
            "'aussom.extern.allowed'.",
            extern_name != NULL ? extern_name : "null");
        parse_throw(ctx, msg);
        return;
    }
    ast_class_set_extern_class_name(cls, extern_name);

    const extern_class *ec = engine_find_extern_class(eng, extern_name);
    if (ec == NULL) {
        snprintf(msg, sizeof(msg), "Extern class '%s' not found.", extern_name);
        parse_throw(ctx, msg);
        return;
    }
    ast_class_set_bound_extern(cls, ec);
}

/* Moves every closure definition accumulated so far onto the class
 * being closed. Ordering makes this correct: every closure inside class
 * A's method bodies reduces before A's classSection does, and A's
 * classSection reduces before any of class B's bodies are parsed, so at
 * each drain point the pending list holds exactly that class's
 * closures. */
static void drain_closures_into(parser_ctx *ctx, ast_node *cls)
{
    ast_node_array pending;
    parser_ctx_drain_pending_closures(ctx, &pending);
    for (size_t i = 0; i < pending.count; i++)
        add_function_checked(ctx, (ast_class *)cls, (ast_funct_def *)pending.items[i]);
}

/* Builds the canonical "compound assignment is just a = a op b"
 * expansion the CUP grammar uses for +=, -=, *=, /=, %=. */
static ast_expression *make_compound_assign(ast_node *left, ast_node *right,
                                            exp_type op)
{
    ast_expression *inner = ast_expression_new();
    inner->left = left;
    inner->right = right;
    inner->e_type = op;

    ast_expression *outer = ast_expression_new();
    outer->left = left;
    outer->right = (ast_node *)inner;
    outer->e_type = EXP_ASSIGNMENT;
    return outer;
}
}

%locations
%define parse.error verbose

/* Pure parser: yylval, yylloc, the lookahead and the error count move
 * out of file-scope globals and become parameters, so two threads can
 * parse at once. `scanner` is flex's yyscan_t (void * here so the
 * grammar does not need flex's header); `ctx` is this parse's context,
 * which used to be reached through the ctx
 * global. */
%define api.pure full
%param       {void *scanner}
%parse-param {parser_ctx *ctx}

%union {
    int64_t                       int_val;
    double                        double_val;
    bool                          bool_val;
    char                         *str_val;
    ast_node                     *node;
    ast_funct_def_args_list      *args_list;
    ast_statement_list           *stmt_list;
    ast_condition_block          *cond_block;
    ast_class                    *class_node;
    ast_list                     *list_node;
    ast_map                      *map_node;
    access_type                   access;
    c_type                        ctype;
    ast_node_array               *node_array;
    ast_node_pair_array          *pair_array;
    string_array                 *str_array;
}

/* --- terminals --------------------------------------------------- */
%token              CLASSDEF
%token              ENUM
%token              INCLUDE
%token              PUBLIC
%token              PRIVATE
%token              STATIC
%token              EXTERN
%token              RETURN
%token              TRY
%token              CATCH
%token              THROW
%token              NEW
%token              KW_NULL
%token              IF
%token              ELSE
%token              SWITCH
%token              CASE
%token              DEFAULT
%token              WHILE
%token              BREAK
%token              FOR
%token              INSTANCEOF
%token              SET
%token              AT

%token              SEMI
%token              COLON
%token              COMMA
%token              LPAREN RPAREN
%token              LBRACKET
%token              RBRACKET
%token              LBRACE
%token              RBRACE

%token <bool_val>   LIT_BOOL
%token <str_val>    LIT_INT
%token <str_val>    LIT_DOUBLE
%token <str_val>    LIT_STRING
%token <str_val>    IDENT
%token <str_val>    AUSSOM_DOC_TOK

%token              EQ
%token              PLEQ MIEQ MUEQ DIEQ MODULUSEQ PLPL MIMI
%token              PL MI MU DI MODULUS FLOORDIV
%token              UMINUS MISSNULL

%token              EQEQ
%token              NOTEQ
%token              LT
%token              GT
%token              LTEQ
%token              GTEQ

%token              NOT
%token              AND
%token              OR
%token              ANDB
%token              ORB
%token              COUNT
%token              ETCETERA
%token              CALLBACK
%token              INSERT

%token              DOT

/* --- nonterminals ------------------------------------------------ */
%type <node>        classBlock classDefExpr classSection classBlocks
%type <node>        memberDefinitionBlock memberDefinition
%type <node>        functionDefinitionBlock functionDefinition
%type <args_list>   functDefArgList
%type <node>        functDefArg
%type <ctype>       functDefPrimType
%type <stmt_list>   instructionListBlock instructionList
%type <node>        instruction topInstruction returnInstruction
%type <node>        tryCatchBlock includeExp
%type <node>        expression operExp indexExp
%type <node_array>  functCallArgList
%type <node>        expNull expBool expInt expDouble expString
%type <node>        expList
%type <node_array>  expListItems
%type <node>        expListIndividual
%type <node>        expMap
%type <pair_array>  expMapItems
%type <node>        expVar expFunctDef expIfBlock expIfBlockList
%type <node_array>  expIfElseBlockList
%type <cond_block>  expIfElseBlock
%type <stmt_list>   expElse
%type <node>        expSwitch expSwitchBlockList
%type <node_array>  expCaseBlockList
%type <cond_block>  expCaseBlock
%type <stmt_list>   expDefault
%type <node>        expWhile expBreak expFor expForExp expEtcetera expCallback
%type <node>        expClosure
%type <node>        expNewList
%type <list_node>   expNewListItems
%type <node>        expNewMap
%type <map_node>    expNewMapItems
%type <node>        expThrow
%type <node>        include externClassRef
%type <bool_val>    isStatic
%type <access>      AccessType
%type <str_array>   extendClassList
%type <node_array>  annotationList annotationArgList
%type <node>        annotation annotationArg
%type <node>        enumBlocks enumRec

/* --- precedence --------------------------------------------------
 *
 * Ordered lowest to highest, mirroring aussom.cup. Tokens that
 * bison flags as having "useless associativity" are declared with
 * %precedence (precedence level only, no associativity) since their
 * left/right choice never resolves a conflict here. ELSE and UMINUS
 * from the CUP grammar are dropped entirely: the if/else productions
 * are explicitly structured (no dangling-else conflict) and there is
 * no unary-minus-of-expression rule (only negative literals, which
 * have no shift/reduce ambiguity), so neither level does any work.
 */
%precedence         THROW
%right              EQ
%right              SET
%right              MIEQ PLEQ DIEQ MODULUSEQ MUEQ INSERT
%left               OR
%left               AND
%left               ORB
%left               ANDB
%left               EQEQ NOTEQ LT GT LTEQ GTEQ
%left               MI PL
%left               DI MU MODULUS FLOORDIV
%precedence         NOT MISSNULL
%precedence         COUNT
/* Pseudo-token (never lexed) used only via %prec on the
 * `expression: expression indexExp` rule. Placing it below DOT and
 * LBRACE makes that reduce lose to extending the index/member chain,
 * which is the shift bison took by default. */
%precedence         INDEX_REDUCE
%right              INSTANCEOF
%right              DOT
%precedence         LBRACE
/* Postfix ++ / -- bind tighter than any prefix or binary operator.
 * Placing them at the top makes the prefix-then-postfix conflicts
 * (e.g. `#a++`) resolve by shift -> `#(a++)`, which is exactly what
 * bison's default did before; declaring the level just makes it
 * explicit and silences the conflict. %precedence (not %left) since
 * the associativity is never consulted. */
%precedence         PLPL MIMI

%start input

%%

input
    : block
    ;

block
    : classBlock
      {
          engine_add_class(ctx->eng, (ast_class *)$1);
          if (engine_parse_exception(ctx->eng) != NULL) ctx->thrown = true;
          ABORT_IF_THROWN();
      }
    | block classBlock
      {
          engine_add_class(ctx->eng, (ast_class *)$2);
          if (engine_parse_exception(ctx->eng) != NULL) ctx->thrown = true;
          ABORT_IF_THROWN();
      }
    | INCLUDE include SEMI
      {
          load_include(ctx, ast_include_get_path((ast_include *)$2));
          ABORT_IF_THROWN();
      }
    | block INCLUDE include SEMI
      {
          load_include(ctx, ast_include_get_path((ast_include *)$3));
          ABORT_IF_THROWN();
      }
    | topInstruction
      {
          parser_ctx_script_or_error(ctx, $1);
      }
    | block topInstruction
      {
          parser_ctx_script_or_error(ctx, $2);
      }
    ;

topInstruction
    : expression SEMI                 { $$ = $1; }
    | returnInstruction               { $$ = $1; }
    | expIfBlock                      { $$ = $1; }
    | expSwitch                       { $$ = $1; }
    | expWhile                        { $$ = $1; }
    | expFor                          { $$ = $1; }
    | tryCatchBlock                   { $$ = $1; }
    ;

include
    : expVar
      {
          ast_include *i = ast_include_new_named(((ast_var *)$1)->base.name);
          SET_INFO_R(i, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)i;
      }
    | include DOT expVar
      {
          ast_include_add_name((ast_include *)$1, ((ast_var *)$3)->base.name);
          $$ = $1;
      }
    ;

classBlock
    : classDefExpr                                  { $$ = $1; }
    | AUSSOM_DOC_TOK classDefExpr
      {
          ast_aussom_doc *ad = doc_node(ctx, $1);
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $2->doc_node = ad;
          $$ = $2;
      }
    | AUSSOM_DOC_TOK annotationList classDefExpr
      {
          ast_aussom_doc *ad = doc_node(ctx, $1);
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $3->doc_node = ad;
          for (size_t i = 0; i < $2->count; i++)
              ast_node_array_add(&$3->annotations, $2->items[i]);
          $$ = $3;
      }
    | annotationList classDefExpr
      {
          SET_INFO_R($2, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          for (size_t i = 0; i < $1->count; i++)
              ast_node_array_add(&$2->annotations, $1->items[i]);
          $$ = $2;
      }
    ;

annotationList
    : annotation
      {
          ast_node_array *a = node_array_new();
          ast_node_array_add(a, $1);
          $$ = a;
      }
    | annotationList annotation
      {
          ast_node_array_add($1, $2);
          $$ = $1;
      }
    ;

annotation
    : AT IDENT
      {
          ast_annotation *aa = ast_annotation_new_named($2);
          SET_INFO_R(aa, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)aa;
      }
    | AT IDENT LPAREN annotationArgList RPAREN
      {
          ast_annotation *aa = ast_annotation_new_named($2);
          SET_INFO_R(aa, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          for (size_t i = 0; i < $4->count; i++)
              ast_node_array_add(&aa->args, $4->items[i]);
          $$ = (ast_node *)aa;
      }
    ;

annotationArgList
    : annotationArg
      {
          ast_node_array *a = node_array_new();
          ast_node_array_add(a, $1);
          $$ = a;
      }
    | annotationArgList COMMA annotationArg
      {
          ast_node_array_add($1, $3);
          $$ = $1;
      }
    ;

annotationArg
    : IDENT EQ LIT_STRING
      {
          ast_annotation_arg *aa = ast_annotation_arg_new_pair($1, $3);
          SET_INFO_R(aa, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)aa;
      }
    ;

classDefExpr
    : isStatic CLASSDEF IDENT classSection
      {
          ast_class *ac = (ast_class *)$4;
          ast_class_set_name(ac, $3);
          SET_INFO_R(ac, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_class_set_static(ac, $1);
          $$ = (ast_node *)ac;
      }
    | isStatic CLASSDEF IDENT COLON extendClassList classSection
      {
          ast_class *ac = (ast_class *)$6;
          ast_class_set_name(ac, $3);
          SET_INFO_R(ac, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_class_set_static(ac, $1);
          ast_class_set_extended_classes(ac, $5->items, $5->count);
          $$ = (ast_node *)ac;
      }
    | isStatic EXTERN CLASSDEF IDENT COLON externClassRef classSection
      {
          ast_class *ac = (ast_class *)$7;
          ast_class_set_name(ac, $4);
          SET_INFO_R(ac, @4.first_line, @4.first_column, @$.last_line, @$.last_column);
          ast_class_set_static(ac, $1);
          ast_class_set_extern(ac, true);
          bind_extern_class(ctx, ac, ast_include_get_extern_class((ast_include *)$6));
          ABORT_IF_THROWN();
          $$ = (ast_node *)ac;
      }
    | isStatic EXTERN CLASSDEF IDENT COLON externClassRef COMMA extendClassList classSection
      {
          ast_class *ac = (ast_class *)$9;
          ast_class_set_name(ac, $4);
          SET_INFO_R(ac, @4.first_line, @4.first_column, @$.last_line, @$.last_column);
          ast_class_set_static(ac, $1);
          ast_class_set_extern(ac, true);
          bind_extern_class(ctx, ac, ast_include_get_extern_class((ast_include *)$6));
          ABORT_IF_THROWN();
          ast_class_set_extended_classes(ac, $8->items, $8->count);
          $$ = (ast_node *)ac;
      }
    | ENUM IDENT LBRACKET enumBlocks RBRACKET
      {
          ast_class *ac = (ast_class *)$4;
          ast_class_set_name(ac, $2);
          SET_INFO_R(ac, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_class_set_static(ac, true);
          $$ = (ast_node *)ac;
      }
    ;

extendClassList
    : IDENT
      {
          string_array *a = string_array_new();
          string_array_add(a, $1);
          $$ = a;
      }
    | extendClassList COMMA IDENT
      {
          string_array_add($1, $3);
          $$ = $1;
      }
    ;

isStatic
    : %empty                                         { $$ = false; }
    | STATIC                                        { $$ = true; }
    ;

externClassRef
    : expVar
      {
          ast_include *i = ast_include_new_named(((ast_var *)$1)->base.name);
          $$ = (ast_node *)i;
      }
    | externClassRef DOT expVar
      {
          ast_include_add_name((ast_include *)$1, ((ast_var *)$3)->base.name);
          $$ = $1;
      }
    ;

classSection
    : LBRACKET RBRACKET
      {
          $$ = (ast_node *)ast_class_new();
          drain_closures_into(ctx, $$);
          ABORT_IF_THROWN();
      }
    | LBRACKET classBlocks RBRACKET
      {
          $$ = $2;
          drain_closures_into(ctx, $$);
          ABORT_IF_THROWN();
      }
    ;

classBlocks
    : memberDefinitionBlock
      {
          ast_class *ac = ast_class_new();
          ast_class_add_member(ac, $1->name, $1);
          $$ = (ast_node *)ac;
      }
    | classBlocks memberDefinitionBlock
      {
          ast_class_add_member((ast_class *)$1, $2->name, $2);
          $$ = $1;
      }
    | functionDefinitionBlock
      {
          ast_class *ac = ast_class_new();
          add_function_checked(ctx, ac, (ast_funct_def *)$1);
          ABORT_IF_THROWN();
          $$ = (ast_node *)ac;
      }
    | classBlocks functionDefinitionBlock
      {
          add_function_checked(ctx, (ast_class *)$1, (ast_funct_def *)$2);
          ABORT_IF_THROWN();
          $$ = $1;
      }
    ;

memberDefinitionBlock
    : memberDefinition                              { $$ = $1; }
    | AUSSOM_DOC_TOK memberDefinition
      {
          ast_aussom_doc *ad = doc_node(ctx, $1);
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $2->doc_node = ad;
          $$ = $2;
      }
    | annotationList memberDefinition
      {
          for (size_t i = 0; i < $1->count; i++)
              ast_node_array_add(&$2->annotations, $1->items[i]);
          $$ = $2;
      }
    | AUSSOM_DOC_TOK annotationList memberDefinition
      {
          ast_aussom_doc *ad = doc_node(ctx, $1);
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $3->doc_node = ad;
          for (size_t i = 0; i < $2->count; i++)
              ast_node_array_add(&$3->annotations, $2->items[i]);
          $$ = $3;
      }
    ;

memberDefinition
    : AccessType expVar SEMI
      {
          $2->access = $1;
          $$ = $2;
      }
    | AccessType expVar EQ expNull SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    | AccessType expVar EQ expBool SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    | AccessType expVar EQ expInt SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    | AccessType expVar EQ expDouble SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    | AccessType expVar EQ expString SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    | AccessType expVar EQ expList SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    | AccessType expVar EQ expMap SEMI
      {
          ast_node_set_name($4, $2->name);
          $4->access = $1;
          $$ = $4;
      }
    ;

enumBlocks
    : enumRec
      {
          ast_class *ac = ast_class_new();
          SET_INFO_R(ac, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_class_add_member(ac, $1->name, $1);
          $$ = (ast_node *)ac;
      }
    | enumBlocks enumRec
      {
          ast_class_add_member((ast_class *)$1, $2->name, $2);
          $$ = $1;
      }
    ;

enumRec
    : expVar SEMI
      {
          ast_string *tmp = ast_string_new_value($1->name);
          ast_node_set_name(&tmp->base, $1->name);
          SET_INFO_R(tmp, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          tmp->base.access = ACCESS_PUBLIC;
          $$ = (ast_node *)tmp;
      }
    | expVar EQ expString SEMI
      {
          ast_string *tmp = ast_string_new_value(((ast_string *)$3)->value);
          ast_node_set_name(&tmp->base, $1->name);
          SET_INFO_R(tmp, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          tmp->base.access = ACCESS_PUBLIC;
          $$ = (ast_node *)tmp;
      }
    ;

functionDefinitionBlock
    : functionDefinition                            { $$ = $1; }
    | AUSSOM_DOC_TOK functionDefinition
      {
          ast_aussom_doc *ad = doc_node(ctx, $1);
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $2->doc_node = ad;
          $$ = $2;
      }
    | annotationList functionDefinition
      {
          for (size_t i = 0; i < $1->count; i++)
              ast_node_array_add(&$2->annotations, $1->items[i]);
          $$ = $2;
      }
    | AUSSOM_DOC_TOK annotationList functionDefinition
      {
          ast_aussom_doc *ad = doc_node(ctx, $1);
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $3->doc_node = ad;
          for (size_t i = 0; i < $2->count; i++)
              ast_node_array_add(&$3->annotations, $2->items[i]);
          $$ = $3;
      }
    ;

functionDefinition
    : AccessType expFunctDef LPAREN functDefArgList RPAREN LBRACKET instructionList RBRACKET
      {
          SET_INFO_R($2, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_funct_def_set_arg_list((ast_funct_def *)$2, $4);
          ast_funct_def_set_instruction_list((ast_funct_def *)$2, $7);
          $2->access = $1;
          $$ = $2;
      }
    | AccessType expFunctDef LPAREN functDefArgList RPAREN LBRACKET RBRACKET
      {
          SET_INFO_R($2, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_funct_def_set_arg_list((ast_funct_def *)$2, $4);
          $2->access = $1;
          $$ = $2;
      }
    | AccessType EXTERN expFunctDef LPAREN functDefArgList RPAREN SEMI
      {
          SET_INFO_R($3, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_funct_def_set_arg_list((ast_funct_def *)$3, $5);
          /* Any script can bind its own class to the same C class, so
           * the C side is the real API and an extern can't be kept
           * private. */
          if ($1 != ACCESS_PUBLIC) {
              char msg[320];
              snprintf(msg, sizeof(msg),
                  "PARSE_ERROR: Extern function '%s' must be public.", $3->name);
              parse_throw_at(ctx, ctx->file_name, $3->line_num, msg);
              YYABORT;
          }
          ast_funct_def_set_extern((ast_funct_def *)$3, true);
          $3->access = $1;
          $$ = $3;
      }
    ;

functDefArgList
    : %empty
      {
          $$ = ast_funct_def_args_list_new();
      }
    | functDefArg
      {
          ast_funct_def_args_list *al = ast_funct_def_args_list_new();
          ast_funct_def_args_list_add(al, $1);
          $$ = al;
      }
    | functDefArgList COMMA functDefArg
      {
          ast_funct_def_args_list_add($1, $3);
          $$ = $1;
      }
    | functDefPrimType functDefArg
      {
          $2->prim_type = $1;
          ast_funct_def_args_list *al = ast_funct_def_args_list_new();
          ast_funct_def_args_list_add(al, $2);
          $$ = al;
      }
    | functDefArgList COMMA functDefPrimType functDefArg
      {
          $4->prim_type = $3;
          ast_funct_def_args_list_add($1, $4);
          $$ = $1;
      }
    ;

functDefArg
    : expVar                                        { $$ = $1; }
    | expVar EQ expNull                             { ast_node_set_name($3, $1->name); $$ = $3; }
    | expVar EQ expBool                             { ast_node_set_name($3, $1->name); $$ = $3; }
    | expVar EQ expInt                              { ast_node_set_name($3, $1->name); $$ = $3; }
    | expVar EQ expDouble                           { ast_node_set_name($3, $1->name); $$ = $3; }
    | expVar EQ expString                           { ast_node_set_name($3, $1->name); $$ = $3; }
    | expVar EQ expList                             { ast_node_set_name($3, $1->name); $$ = $3; }
    | expVar EQ expMap                              { ast_node_set_name($3, $1->name); $$ = $3; }
    | expEtcetera                                   { $$ = $1; }
    ;

functDefPrimType
    : IDENT
      {
          if (strcmp($1, "bool") == 0)          $$ = C_TYPE_BOOL;
          else if (strcmp($1, "int") == 0)      $$ = C_TYPE_INT;
          else if (strcmp($1, "double") == 0)   $$ = C_TYPE_DOUBLE;
          else if (strcmp($1, "string") == 0)   $$ = C_TYPE_STRING;
          else if (strcmp($1, "list") == 0)     $$ = C_TYPE_LIST;
          else if (strcmp($1, "map") == 0)      $$ = C_TYPE_MAP;
          else if (strcmp($1, "object") == 0)   $$ = C_TYPE_OBJECT;
          else if (strcmp($1, "callback") == 0) $$ = C_TYPE_CALLBACK;
          else {
              char pmsg[192];
              snprintf(pmsg, sizeof(pmsg),
                  "PARSE_ERROR: Expecting primative identifier but found '%s' instead.", $1);
              parse_throw_at(ctx, ctx->file_name, @1.first_line, pmsg);
              YYABORT;
          }
      }
    ;

instructionList
    : instruction
      {
          ast_statement_list *il = ast_statement_list_new();
          ast_statement_list_add(il, $1);
          $$ = il;
      }
    | instructionList instruction
      {
          ast_statement_list_add($1, $2);
          $$ = $1;
      }
    ;

instruction
    : expression SEMI                               { $$ = $1; }
    | returnInstruction                             { $$ = $1; }
    | expIfBlock                                    { $$ = $1; }
    | expSwitch                                     { $$ = $1; }
    | expWhile                                      { $$ = $1; }
    | expFor                                        { $$ = $1; }
    | tryCatchBlock                                 { $$ = $1; }
    | includeExp                                    { $$ = $1; }
    ;

includeExp
    : INCLUDE include SEMI
      {
          ast_expression *e = ast_expression_new_with_left($2);
          SET_INFO_R(e, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          e->e_type = EXP_INCLUDE;
          $$ = (ast_node *)e;
      }
    ;

expression
    : operExp                                       { $$ = $1; }
    | NEW expVar LPAREN functCallArgList RPAREN
      {
          ast_new_inst *ni = ast_new_inst_new_named(((ast_var *)$2)->base.name);
          SET_INFO_R(ni, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_funct_def_args_list *nl = ast_funct_def_args_list_new();
          for (size_t i = 0; i < $4->count; i++)
              ast_funct_def_args_list_add(nl, $4->items[i]);
          ast_new_inst_set_args(ni, nl);
          $$ = (ast_node *)ni;
      }
    | expNewList                                    { $$ = $1; }
    | expNewMap                                     { $$ = $1; }
    | expCallback                                   { $$ = $1; }
    | expClosure                                    { $$ = $1; }
    | expression INSERT expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3;
          e->e_type = EXP_INSERT;
          $$ = (ast_node *)e;
      }
    | COUNT expression
      {
          ast_expression *e = ast_expression_new_with_left($2);
          SET_INFO_R(e, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          e->e_type = EXP_COUNT;
          $$ = (ast_node *)e;
      }
    | expVar LPAREN functCallArgList RPAREN
      {
          ast_funct_call *afc = ast_funct_call_new_named(((ast_var *)$1)->base.name);
          SET_INFO_R(afc, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_funct_def_args_list *nl = ast_funct_def_args_list_new();
          for (size_t i = 0; i < $3->count; i++)
              ast_funct_def_args_list_add(nl, $3->items[i]);
          ast_funct_call_set_args(afc, nl);
          $$ = (ast_node *)afc;
      }
    | expression DOT expression
      {
          $1->child = $3;
          $$ = $1;
      }
    | expression INSTANCEOF expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3;
          e->e_type = EXP_INSTANCEOF;
          $$ = (ast_node *)e;
      }
    | expression indexExp  %prec INDEX_REDUCE
      {
          $1->child = $2;
          $$ = $1;
      }
    | expVar
      {
          ast_obj *ao = ast_obj_new();
          SET_INFO_R(ao, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_node_set_name(&ao->base, ((ast_var *)$1)->base.name);
          $$ = (ast_node *)ao;
      }
    | expNull                                       { $$ = $1; }
    | expBool                                       { $$ = $1; }
    | expInt                                        { $$ = $1; }
    | expDouble                                     { $$ = $1; }
    | expString                                     { $$ = $1; }
    | expBreak                                      { $$ = $1; }
    | expThrow                                      { $$ = $1; }
    | LPAREN expression RPAREN                      { $$ = $2; }
    ;

indexExp
    : LBRACE expression RBRACE
      {
          ast_obj *aobj = ast_obj_new();
          SET_INFO_R(aobj, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_obj_set_index(aobj, $2);
          $$ = (ast_node *)aobj;
      }
    | indexExp LBRACE expression RBRACE
      {
          ast_obj *aobj = ast_obj_new();
          SET_INFO_R(aobj, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_obj_set_index(aobj, $3);
          ast_node_append_child($1, (ast_node *)aobj);
          $$ = $1;
      }
    | indexExp DOT expression
      {
          ast_node_append_child($1, $3);
          $$ = $1;
      }
    ;

operExp
    : expression PL expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_ADD;
          $$ = (ast_node *)e;
      }
    | expression MI expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_SUBTRACT;
          $$ = (ast_node *)e;
      }
    | expression MU expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_MULTIPLY;
          $$ = (ast_node *)e;
      }
    | expression DI expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_DIVIDE;
          $$ = (ast_node *)e;
      }
    | expression MODULUS expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_MODULUS;
          $$ = (ast_node *)e;
      }
    | expression FLOORDIV expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_FLOORDIV;
          $$ = (ast_node *)e;
      }
    | expression SET expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_SET;
          $$ = (ast_node *)e;
      }
    | expression EQ expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_ASSIGNMENT;
          $$ = (ast_node *)e;
      }
    | expression PLEQ expression
      {
          ast_expression *e = make_compound_assign($1, $3, EXP_ADD);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression MIEQ expression
      {
          ast_expression *e = make_compound_assign($1, $3, EXP_SUBTRACT);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression MUEQ expression
      {
          ast_expression *e = make_compound_assign($1, $3, EXP_MULTIPLY);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression DIEQ expression
      {
          ast_expression *e = make_compound_assign($1, $3, EXP_DIVIDE);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression MODULUSEQ expression
      {
          ast_expression *e = make_compound_assign($1, $3, EXP_MODULUS);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression PLPL
      {
          ast_int *ai = ast_int_new_value(1);
          SET_INFO_R(ai, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_expression *e = make_compound_assign($1, (ast_node *)ai, EXP_ADD);
          SET_INFO_R(e, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression MIMI
      {
          ast_int *ai = ast_int_new_value(1);
          SET_INFO_R(ai, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_expression *e = make_compound_assign($1, (ast_node *)ai, EXP_SUBTRACT);
          SET_INFO_R(e, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)e;
      }
    | expression EQEQ expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_EQEQ;
          $$ = (ast_node *)e;
      }
    | expression NOTEQ expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_NOTEQ;
          $$ = (ast_node *)e;
      }
    | expression LT expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_LT;
          $$ = (ast_node *)e;
      }
    | expression GT expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_GT;
          $$ = (ast_node *)e;
      }
    | expression LTEQ expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_LTEQ;
          $$ = (ast_node *)e;
      }
    | expression GTEQ expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_GTEQ;
          $$ = (ast_node *)e;
      }
    | NOT expression
      {
          ast_expression *e = ast_expression_new_with_left($2);
          SET_INFO_R(e, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          e->e_type = EXP_NOT;
          $$ = (ast_node *)e;
      }
    | MISSNULL expression
      {
          ast_expression *e = ast_expression_new_with_left($2);
          SET_INFO_R(e, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          e->e_type = EXP_MISSNULL;
          $$ = (ast_node *)e;
      }
    | expression AND expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_AND;
          $$ = (ast_node *)e;
      }
    | expression OR expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_OR;
          $$ = (ast_node *)e;
      }
    | expression ANDB expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_ANDB;
          $$ = (ast_node *)e;
      }
    | expression ORB expression
      {
          ast_expression *e = ast_expression_new_with_left($1);
          SET_INFO_R(e, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          e->right = $3; e->e_type = EXP_ORB;
          $$ = (ast_node *)e;
      }
    ;

returnInstruction
    : RETURN SEMI
      {
          ast_return *ar = ast_return_new();
          SET_INFO_R(ar, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ar;
      }
    | RETURN expression SEMI
      {
          ast_return *ar = ast_return_new();
          ast_return_set_value(ar, $2);
          SET_INFO_R(ar, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ar;
      }
    ;

tryCatchBlock
    : TRY instructionListBlock CATCH LPAREN expVar RPAREN instructionListBlock
      {
          ast_trycatch *atc = ast_trycatch_new();
          SET_INFO_R(atc, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_node_set_name(&atc->base, $5->name);
          ast_trycatch_set_try_inst_list(atc, $2);
          ast_trycatch_set_catch_inst_list(atc, $7);
          $$ = (ast_node *)atc;
      }
    ;

instructionListBlock
    : LBRACKET RBRACKET                             { $$ = ast_statement_list_new(); }
    | LBRACKET instructionList RBRACKET             { $$ = $2; }
    ;

expIfBlock
    : expIfBlockList                                { $$ = $1; }
    | expIfBlockList expIfElseBlockList
      {
          ast_ifelse *aie = (ast_ifelse *)$1;
          for (size_t i = 0; i < $2->count; i++)
              ast_ifelse_add_else_if(aie, (ast_condition_block *)$2->items[i]);
          $$ = (ast_node *)aie;
      }
    | expIfBlockList expElse
      {
          ast_ifelse *aie = (ast_ifelse *)$1;
          aie->else_instruction_list = $2;
          $$ = (ast_node *)aie;
      }
    | expIfBlockList expIfElseBlockList expElse
      {
          ast_ifelse *aie = (ast_ifelse *)$1;
          for (size_t i = 0; i < $2->count; i++)
              ast_ifelse_add_else_if(aie, (ast_condition_block *)$2->items[i]);
          aie->else_instruction_list = $3;
          $$ = (ast_node *)aie;
      }
    ;

expIfBlockList
    : IF LPAREN expression RPAREN LBRACKET RBRACKET
      {
          ast_ifelse *aie = ast_ifelse_new();
          SET_INFO_R(aie, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_condition_block *acb = ast_condition_block_new();
          ast_condition_block_set_expression(acb, $3);
          ast_ifelse_set_if_condition(aie, acb);
          $$ = (ast_node *)aie;
      }
    | IF LPAREN expression RPAREN LBRACKET instructionList RBRACKET
      {
          ast_ifelse *aie = ast_ifelse_new();
          SET_INFO_R(aie, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_condition_block *acb = ast_condition_block_new();
          ast_condition_block_set_expression(acb, $3);
          acb->instruction_list = $6;
          ast_ifelse_set_if_condition(aie, acb);
          $$ = (ast_node *)aie;
      }
    ;

expIfElseBlockList
    : expIfElseBlock
      {
          ast_node_array *a = node_array_new();
          ast_node_array_add(a, (ast_node *)$1);
          $$ = a;
      }
    | expIfElseBlockList expIfElseBlock
      {
          ast_node_array_add($1, (ast_node *)$2);
          $$ = $1;
      }
    ;

expIfElseBlock
    : ELSE IF LPAREN expression RPAREN LBRACKET RBRACKET
      {
          ast_condition_block *acb = ast_condition_block_new();
          SET_INFO_R(acb, @4.first_line, @4.first_column, @$.last_line, @$.last_column);
          ast_condition_block_set_expression(acb, $4);
          $$ = acb;
      }
    | ELSE IF LPAREN expression RPAREN LBRACKET instructionList RBRACKET
      {
          ast_condition_block *acb = ast_condition_block_new();
          SET_INFO_R(acb, @4.first_line, @4.first_column, @$.last_line, @$.last_column);
          ast_condition_block_set_expression(acb, $4);
          acb->instruction_list = $7;
          $$ = acb;
      }
    ;

expElse
    : ELSE LBRACKET RBRACKET                        { $$ = ast_statement_list_new(); }
    | ELSE LBRACKET instructionList RBRACKET        { $$ = $3; }
    ;

expSwitch
    : SWITCH LPAREN expression RPAREN LBRACKET expSwitchBlockList RBRACKET
      {
          ast_switch *as = (ast_switch *)$6;
          SET_INFO_R(as, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_switch_set_expression(as, $3);
          $$ = (ast_node *)as;
      }
    | SWITCH LPAREN expression RPAREN LBRACKET RBRACKET
      {
          ast_switch *as = ast_switch_new();
          SET_INFO_R(as, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_switch_set_expression(as, $3);
          $$ = (ast_node *)as;
      }
    ;

expSwitchBlockList
    : expCaseBlockList
      {
          ast_switch *as = ast_switch_new();
          for (size_t i = 0; i < $1->count; i++)
              ast_switch_add_case(as, (ast_condition_block *)$1->items[i]);
          $$ = (ast_node *)as;
      }
    | expCaseBlockList expDefault
      {
          ast_switch *as = ast_switch_new();
          for (size_t i = 0; i < $1->count; i++)
              ast_switch_add_case(as, (ast_condition_block *)$1->items[i]);
          as->default_list = $2;
          $$ = (ast_node *)as;
      }
    ;

expCaseBlockList
    : expCaseBlock
      {
          ast_node_array *a = node_array_new();
          ast_node_array_add(a, (ast_node *)$1);
          $$ = a;
      }
    | expCaseBlockList expCaseBlock
      {
          ast_node_array_add($1, (ast_node *)$2);
          $$ = $1;
      }
    ;

expCaseBlock
    : CASE expression COLON LBRACKET instructionList RBRACKET
      {
          ast_condition_block *acb = ast_condition_block_new();
          SET_INFO_R(acb, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_condition_block_set_expression(acb, $2);
          acb->instruction_list = $5;
          $$ = acb;
      }
    | CASE expression COLON LBRACKET RBRACKET
      {
          ast_condition_block *acb = ast_condition_block_new();
          SET_INFO_R(acb, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_condition_block_set_expression(acb, $2);
          $$ = acb;
      }
    ;

expDefault
    : DEFAULT COLON LBRACKET instructionList RBRACKET   { $$ = $4; }
    | DEFAULT COLON LBRACKET RBRACKET                   { $$ = ast_statement_list_new(); }
    ;

expWhile
    : WHILE LPAREN expression RPAREN LBRACKET instructionList RBRACKET
      {
          ast_while *aw = ast_while_new();
          SET_INFO_R(aw, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_while_set_expr(aw, $3);
          aw->instructions = $6;
          $$ = (ast_node *)aw;
      }
    | WHILE LPAREN expression RPAREN LBRACKET RBRACKET
      {
          ast_while *aw = ast_while_new();
          SET_INFO_R(aw, @3.first_line, @3.first_column, @$.last_line, @$.last_column);
          ast_while_set_expr(aw, $3);
          $$ = (ast_node *)aw;
      }
    ;

expFor
    : FOR LPAREN expForExp SEMI expForExp SEMI expForExp RPAREN LBRACKET instructionList RBRACKET
      {
          ast_for *af = ast_for_new();
          SET_INFO_R(af, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_for_set_expr_init(af, $3);
          ast_for_set_expr_cond(af, $5);
          ast_for_set_expr_inc(af, $7);
          af->instructions = $10;
          $$ = (ast_node *)af;
      }
    | FOR LPAREN expForExp SEMI expForExp SEMI expForExp RPAREN LBRACKET RBRACKET
      {
          ast_for *af = ast_for_new();
          SET_INFO_R(af, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_for_set_expr_init(af, $3);
          ast_for_set_expr_cond(af, $5);
          ast_for_set_expr_inc(af, $7);
          $$ = (ast_node *)af;
      }
    | FOR LPAREN expVar COLON expression RPAREN LBRACKET instructionList RBRACKET
      {
          ast_for *af = ast_for_new();
          SET_INFO_R(af, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_for_set_is_for_each(af, true);
          ast_for_set_each_var(af, $3);
          ast_for_set_each_expr(af, $5);
          af->instructions = $8;
          $$ = (ast_node *)af;
      }
    | FOR LPAREN expVar COLON expression RPAREN LBRACKET RBRACKET
      {
          ast_for *af = ast_for_new();
          SET_INFO_R(af, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_for_set_is_for_each(af, true);
          ast_for_set_each_var(af, $3);
          ast_for_set_each_expr(af, $5);
          $$ = (ast_node *)af;
      }
    ;

expForExp
    : %empty                                         { $$ = (ast_node *)ast_bool_new_value(true); }
    | expression                                    { $$ = $1; }
    ;

functCallArgList
    : %empty                                         { $$ = node_array_new(); }
    | expression
      {
          ast_node_array *a = node_array_new();
          ast_node_array_add(a, $1);
          $$ = a;
      }
    | functCallArgList COMMA expression
      {
          ast_node_array_add($1, $3);
          $$ = $1;
      }
    ;

AccessType
    : %empty                                         { $$ = ACCESS_PRIVATE; }
    | PUBLIC                                        { $$ = ACCESS_PUBLIC; }
    | PRIVATE                                       { $$ = ACCESS_PRIVATE; }
    ;

expNull
    : KW_NULL
      {
          ast_null *an = ast_null_new();
          SET_INFO_R(an, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)an;
      }
    ;

expBool
    : LIT_BOOL
      {
          ast_bool *ab = ast_bool_new_value($1);
          SET_INFO_R(ab, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ab;
      }
    ;

expInt
    : LIT_INT
      {
          const char *d = int_lit_digits($1);
          if (cmp_digits(d, "9223372036854775807") > 0) {
              char *m = AUSSOM_ALLOC_ATOMIC(strlen(d) + 128);
              sprintf(m, "PARSE_ERROR: Integer out of bounds. Integer max size "
                         "is 9223372036854775807. <%s>", d);
              parse_throw_at(ctx, ctx->file_name, @1.first_line, m);
              YYABORT;
          }
          ast_int *ai = ast_int_new_value(int_lit_value(d, false));
          SET_INFO_R(ai, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ai;
      }
    | MI LIT_INT
      {
          const char *d = int_lit_digits($2);
          if (cmp_digits(d, "9223372036854775808") > 0) {
              char *m = AUSSOM_ALLOC_ATOMIC(strlen(d) + 128);
              sprintf(m, "PARSE_ERROR: Integer out of bounds. Integer min size "
                         "is -9223372036854775808. <-%s>", d);
              parse_throw_at(ctx, ctx->file_name, @2.first_line, m);
              YYABORT;
          }
          ast_int *ai = ast_int_new_value(int_lit_value(d, true));
          SET_INFO_R(ai, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ai;
      }
    ;

expDouble
    : LIT_DOUBLE
      {
          dec_lit dl = dec_lit_parse($1);
          if (dec_lit_over_max(dl)) {
              char *v = dec_lit_to_string(dl, false);
              char *m = AUSSOM_ALLOC_ATOMIC(strlen(v) + 128);
              sprintf(m, "PARSE_ERROR: Double out of bounds. Double max size "
                         "is 1.7976931348623157E308. <%s>", v);
              parse_throw_at(ctx, ctx->file_name, @1.first_line, m);
              YYABORT;
          }
          ast_double *ad = ast_double_new_value(strtod($1, NULL));
          SET_INFO_R(ad, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ad;
      }
    | MI LIT_DOUBLE
      {
          /* Java's message here names Double.MIN_NORMAL, not the most
           * negative double; ported as written. */
          dec_lit dl = dec_lit_parse($2);
          if (dec_lit_over_max(dl)) {
              char *v = dec_lit_to_string(dec_lit_times_neg_one(dl), true);
              char *m = AUSSOM_ALLOC_ATOMIC(strlen(v) + 128);
              sprintf(m, "PARSE_ERROR: Double out of bounds. Double min size "
                         "is 2.2250738585072014E-308. <%s>", v);
              parse_throw_at(ctx, ctx->file_name, @2.first_line, m);
              YYABORT;
          }
          /* BigDecimal has no negative zero, so Java's -0.0 is 0.0. */
          double dv = strtod($2, NULL);
          ast_double *ad = ast_double_new_value(dv == 0.0 ? 0.0 : -dv);
          SET_INFO_R(ad, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ad;
      }
    ;

expString
    : LIT_STRING
      {
          ast_string *as = ast_string_new_value($1);
          SET_INFO_R(as, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)as;
      }
    ;

expList
    : LBRACE expListItems RBRACE
      {
          ast_list *al = ast_list_new();
          for (size_t i = 0; i < $2->count; i++) ast_list_add(al, $2->items[i]);
          SET_INFO_R(al, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)al;
      }
    | LBRACE RBRACE
      {
          ast_list *al = ast_list_new();
          SET_INFO_R(al, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)al;
      }
    ;

expListItems
    : expListIndividual
      {
          ast_node_array *a = node_array_new();
          ast_node_array_add(a, $1);
          $$ = a;
      }
    | expListItems COMMA expListIndividual
      {
          ast_node_array_add($1, $3);
          $$ = $1;
      }
    ;

expListIndividual
    : expBool                                       { $$ = $1; }
    | expInt                                        { $$ = $1; }
    | expDouble                                     { $$ = $1; }
    | expString                                     { $$ = $1; }
    | expList                                       { $$ = $1; }
    | expMap                                        { $$ = $1; }
    ;

expMap
    : LBRACKET expMapItems RBRACKET
      {
          ast_map *ad = ast_map_new();
          for (size_t i = 0; i < $2->count; i++)
              ast_map_add(ad, $2->items[i].key, $2->items[i].value);
          SET_INFO_R(ad, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ad;
      }
    | LBRACKET RBRACKET
      {
          ast_map *am = ast_map_new();
          SET_INFO_R(am, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)am;
      }
    ;

expMapItems
    : expString COLON expListIndividual
      {
          ast_node_pair_array *a = pair_array_new();
          ast_node_pair_array_add(a, $1, $3);
          $$ = a;
      }
    | expVar COLON expListIndividual
      {
          ast_node_pair_array *a = pair_array_new();
          ast_node_pair_array_add(a, $1, $3);
          $$ = a;
      }
    | expMapItems COMMA expString COLON expListIndividual
      {
          ast_node_pair_array_add($1, $3, $5);
          $$ = $1;
      }
    | expMapItems COMMA expVar COLON expListIndividual
      {
          ast_node_pair_array_add($1, $3, $5);
          $$ = $1;
      }
    ;

expVar
    : IDENT
      {
          ast_var *av = ast_var_new();
          SET_INFO_R(av, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_node_set_name(&av->base, $1);
          $$ = (ast_node *)av;
      }
    ;

expFunctDef
    : IDENT
      {
          ast_funct_def *afd = ast_funct_def_new_named($1);
          SET_INFO_R(afd, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)afd;
      }
    ;

expBreak
    : BREAK
      {
          ast_break *ab = ast_break_new();
          SET_INFO_R(ab, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)ab;
      }
    ;

expEtcetera
    : ETCETERA
      {
          ast_etcetera *etc = ast_etcetera_new();
          SET_INFO_R(etc, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)etc;
      }
    ;

expCallback
    : CALLBACK IDENT
      {
          ast_callback *cb = ast_callback_new_named($2);
          SET_INFO_R(cb, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)cb;
      }
    ;

/* Closure definition site. The function definition is hoisted onto the
 * enclosing class at parse time as a private funct def with the closure
 * flag set (see drain_closures_into); the node left in the expression
 * only records the name and, when evaluated, produces a callback that
 * captures the defining invocation's locals.
 *
 * The `::` prefix is required and is what keeps this LALR(1): after
 * CALLBACK IDENT the parser either shifts `(` for a closure or reduces
 * expCallback for a plain `::name`, and `(` never follows a completed
 * ::name expression anywhere in the grammar. A bare `name(int x) {`
 * header would be a reduce/reduce conflict against a function call. */
expClosure
    : CALLBACK expFunctDef LPAREN functDefArgList RPAREN LBRACKET instructionList RBRACKET
      {
          ast_funct_def *fd = (ast_funct_def *)$2;
          SET_INFO_R(fd, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_funct_def_set_arg_list(fd, $4);
          ast_funct_def_set_instruction_list(fd, $7);
          fd->base.access = ACCESS_PRIVATE;
          ast_funct_def_set_closure(fd, true);
          ast_node_array_add(&ctx->pending_closures, &fd->base);

          ast_callback *cl = ast_closure_new_named(fd->base.name);
          SET_INFO_R(cl, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)cl;
      }
    | CALLBACK expFunctDef LPAREN functDefArgList RPAREN LBRACKET RBRACKET
      {
          ast_funct_def *fd = (ast_funct_def *)$2;
          SET_INFO_R(fd, @2.first_line, @2.first_column, @$.last_line, @$.last_column);
          ast_funct_def_set_arg_list(fd, $4);
          fd->base.access = ACCESS_PRIVATE;
          ast_funct_def_set_closure(fd, true);
          ast_node_array_add(&ctx->pending_closures, &fd->base);

          ast_callback *cl = ast_closure_new_named(fd->base.name);
          SET_INFO_R(cl, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          $$ = (ast_node *)cl;
      }
    ;

expNewList
    : LBRACE expNewListItems RBRACE                 { $$ = (ast_node *)$2; }
    ;

expNewListItems
    : %empty                                         { $$ = ast_list_new(); }
    | expression
      {
          ast_list *al = ast_list_new();
          ast_list_add(al, $1);
          $$ = al;
      }
    | expNewListItems COMMA expression
      {
          ast_list_add($1, $3);
          $$ = $1;
      }
    ;

expNewMap
    : LBRACKET expNewMapItems RBRACKET              { $$ = (ast_node *)$2; }
    ;

expNewMapItems
    : %empty                                         { $$ = ast_map_new(); }
    | expression COLON expression
      {
          ast_map *am = ast_map_new();
          SET_INFO_R(am, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_map_add(am, $1, $3);
          $$ = am;
      }
    | expNewMapItems COMMA expression COLON expression
      {
          ast_map_add($1, $3, $5);
          $$ = $1;
      }
    ;

expThrow
    : THROW expression
      {
          ast_throw *at = ast_throw_new();
          SET_INFO_R(at, @1.first_line, @1.first_column, @$.last_line, @$.last_column);
          ast_throw_set_expression(at, $2);
          $$ = (ast_node *)at;
      }
    ;

%%

static void yyerror(YYLTYPE *yyllocp, void *scanner, parser_ctx *ctx,
                    const char *fmt, ...)
{
    (void)scanner;
    va_list ap;
    char msg[512];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (ctx != NULL) {
        /* parser.report_error: Java logs its own message, not the
         * parser generator's, naming the offending token's line and
         * column. yylloc holds that token's position. There is no
         * error recovery in the grammar, so the parse stops here, as
         * report_error's done_parsing() makes Java's stop. */
        char jmsg[128];
        snprintf(jmsg, sizeof(jmsg),
            "PARSE_ERROR: Unknown symbol found at line %d column %d.",
            yyllocp->first_line, yyllocp->first_column);
        ctx->syntax_errors = true;
        engine_set_parse_error(ctx->eng);
        /* Structured diagnostic alongside -- never instead of -- the
         * console message, so tooling can read the position back as
         * integers. */
        engine_add_parse_diagnostic(ctx->eng, ctx->file_name,
            yyllocp->first_line, yyllocp->first_column, jmsg);
        char text[640];
        snprintf(text, sizeof(text), "%s [%d]: %s",
                 ctx->file_name != NULL ? ctx->file_name : "",
                 yyllocp->first_line, jmsg);
        engine_err(ctx->eng, text);
    } else {
        parse_report(NULL, NULL, yyllocp != NULL ? yyllocp->first_line : 0, msg);
    }
}

/* Buffer-input glue used by aussom_parse_string. We point the scanner
 * at a memory FILE* via aussom_memstream_open_read so flex can keep its
 * normal FILE-based input path.
 *
 * Every parse builds its own scanner and its own context and passes
 * both explicitly, so nothing about this parse is reachable from
 * another one. That is what lets two engines parse at the same time. */
static int parse_with_yyin(engine *eng, FILE *fp, const char *file_name,
                            ast_statement_list *script_target, int line_offset,
                            ast_node_array *leftovers)
{
    engine_clear_parse_exception(eng);
    parser_ctx *ctx = parser_ctx_new(eng, file_name);
    ctx->script_stmt_target = script_target;
    ctx->line_offset = line_offset;
    if (leftovers != NULL) ast_node_array_init(leftovers);

    void *scanner = aussom_lex_begin(ctx, fp);
    if (scanner == NULL) {
        engine_set_parse_error(eng);
        engine_add_parse_diagnostic(eng, file_name, PARSE_DIAG_NO_POSITION,
            PARSE_DIAG_NO_POSITION, "Could not create a scanner for this parse.");
        engine_err(eng, "aussom: could not create a scanner for this parse.");
        return -1;
    }

    int rc = yyparse(scanner, ctx);
    if (ctx->thrown) {
        aussom_lex_end(scanner);
        return -1;
    }

    /* Closures written in top-level statements never pass a
     * classSection, so they are still pending here. Handed back to
     * aussom_parse_statements, which hoists them onto the script class,
     * as Engine.parseStatements does. A file or string parse has no
     * top-level statements to hold one, and drops them as Java's
     * parseFile and parseString do. */
    if (leftovers != NULL) parser_ctx_drain_pending_closures(ctx, leftovers);

    aussom_lex_end(scanner);
    return (rc == 0 && !ctx->syntax_errors && !eng->parse_error) ? 0 : -1;
}

bool aussom_parse_file(engine *eng, const char *file_path)
{
    /* Engine.parseFile throws when the file can't be read (Java's
     * FileNotFoundException, "<path> (<reason>)") or is over the source
     * byte limit. Like every error Java throws out of a parse, these
     * are left for the caller rather than logged or flagged. */
    engine_clear_parse_exception(eng);
    char msg[4352];
    FILE *fp = fopen(file_path, "r");
    if (fp == NULL) {
        snprintf(msg, sizeof(msg), "%s (%s)", file_path, strerror(errno));
        engine_set_parse_exception(eng, msg, msg);
        return false;
    }

    /* Source size limit, checked against the file length before a
     * single token is read. Source is the one parse cost worth
     * bounding: the file is read whole before the parser sees anything,
     * and the definitions it becomes are several times the size of the
     * text. Refusing it here rather than after loading is the point.
     * 0 means no limit, which is the default. */
    int64_t max_src = engine_get_limits(eng)->source_bytes;
    if (max_src > 0) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            long len = ftell(fp);
            rewind(fp);
            if (len > 0 && (int64_t)len > max_src) {
                snprintf(msg, sizeof(msg),
                    "Engine.parseFile(): Source file '%s' is %ld bytes, over "
                    "the limit of %lld bytes.",
                    file_path, len, (long long)max_src);
                engine_set_parse_exception(eng, msg, msg);
                fclose(fp);
                return false;
            }
        }
    }

    int rc = parse_with_yyin(eng, fp, file_path, NULL, 0, NULL);
    fclose(fp);
    return rc == 0;
}

bool aussom_parse_string(engine *eng, const char *source, const char *file_name)
{
    FILE *fp = aussom_memstream_open_read(source, strlen(source));
    if (fp == NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "aussom: cannot read source for '%s'", file_name);
        engine_err(eng, msg);
        engine_set_parse_error(eng);
        return false;
    }
    int rc = parse_with_yyin(eng, fp, file_name, NULL, 0, NULL);
    fclose(fp);
    return rc == 0;
}

bool aussom_parse_statements(engine *eng, const char *file_name,
                             const char *source, int line_offset,
                             ast_statement_list *target)
{
    /* One submission is one parse: drop the diagnostics of the one
     * before, so the caller reads only its own. */
    engine_clear_parse_diagnostics(eng);

    FILE *fp = aussom_memstream_open_read(source, strlen(source));
    if (fp == NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "aussom: cannot read source for '%s'", file_name);
        engine_err(eng, msg);
        engine_set_parse_error(eng);
        return false;
    }
    ast_node_array leftovers;
    int rc = parse_with_yyin(eng, fp, file_name, target, line_offset, &leftovers);
    fclose(fp);

    /* Engine.parseStatements catches what the parse threw and turns it
     * into an ordinary parse error: logged, recorded as a file-level
     * diagnostic and flagged, so a caller of script mode reads every
     * failure the same way. */
    const char *thrown = engine_parse_exception(eng);
    if (thrown != NULL) {
        engine_err(eng, thrown);
        engine_add_parse_diagnostic(eng, file_name, PARSE_DIAG_NO_POSITION,
                                    PARSE_DIAG_NO_POSITION, thrown);
        engine_set_parse_error(eng);
    }

    /* Closures defined in top-level statements are hoisted onto the
     * script class. With no script class to receive them there is
     * nowhere for the definition to live, so that is a parse error. */
    if (leftovers.count > 0) {
        ast_class *script_class = engine_get_script_class(eng);
        char text[1024];
        if (script_class != NULL) {
            for (size_t i = 0; i < leftovers.count; i++) {
                ast_funct_def *c = (ast_funct_def *)leftovers.items[i];
                char *sig = NULL;
                ast_funct_def *prior = ast_class_add_function(script_class, c, &sig);
                if (prior == NULL) continue;
                /* Duplicate closure name and signature on the script
                 * class: astClass.addFunction's error. */
                size_t n = strlen(c->base.name) + strlen(sig) + 512;
                char *msg = AUSSOM_ALLOC_ATOMIC(n);
                snprintf(msg, n,
                    "Duplicate method overload '%s(%s)' declared at %s:%d; first "
                    "declared at %s:%d.",
                    c->base.name, sig,
                    c->base.file_name != NULL ? c->base.file_name : "",
                    c->base.line_num,
                    prior->base.file_name != NULL ? prior->base.file_name : "",
                    prior->base.line_num);
                engine_err(eng, msg);
                engine_add_parse_diagnostic(eng, file_name, c->base.line_num,
                                            c->base.col_num, msg);
                engine_set_parse_error(eng);
                rc = -1;
                break;
            }
        } else {
            ast_node *orphan = leftovers.items[0];
            static const char msg[] = "PARSE_ERROR: Closure defined outside a "
                "class with no script class to receive it.";
            snprintf(text, sizeof(text), "%s [%d]: %s", file_name,
                     orphan->line_num, msg);
            engine_err(eng, text);
            engine_add_parse_diagnostic(eng, file_name, orphan->line_num,
                                        orphan->col_num, msg);
            engine_set_parse_error(eng);
            rc = -1;
        }
    }
    return rc == 0;
}
