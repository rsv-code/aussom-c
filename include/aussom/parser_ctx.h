/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_PARSER_CTX_H
#define AUSSOM_PARSER_CTX_H

#include <stdbool.h>

#include "aussom/ast_statement_list.h"
#include "aussom/engine.h"

/* Bag of state that the bison-generated parser needs while it runs.
 * Mirrors the CUP `parser code` fields: the engine, the source file
 * name (for error messages and ast_node parser_info), the
 * load-extern flag (currently informational only; the C port does
 * not perform host-language class loading), the script-mode
 * statement target, and a sticky syntax-errors flag.
 *
 * Passed to the parser with %parse-param, one per parse, which is what
 * makes the parser re-entrant: an include parsed from inside a grammar
 * action gets its own context and scanner. */
typedef struct parser_ctx {
    engine *eng;
    char *file_name;
    bool load_extern_classes;
    /* When non-NULL, top-level statements (anything outside a class
     * block) are appended here. When NULL, the parser raises a
     * PARSE_ERROR via parser_ctx_script_or_error. */
    ast_statement_list *script_stmt_target;
    bool syntax_errors;
    /* Added to every line the scanner reports, so the first source line
     * reports as line_offset + 1. Ports Lexer.setLineOffset; 0 except
     * for script-mode parses (engine_parse_script_line). */
    int line_offset;
    /* Set when the parse stopped with an error aussom-base throws. The
     * error itself is on the engine (engine_parse_exception). */
    bool thrown;
    /* Closure definitions (::name(args) { ... }) reduce inside function
     * bodies, long before the enclosing classSection does, so they
     * cannot be attached to their class from the expClosure action.
     * They accumulate here and are drained into the class by the
     * classSection actions. Leftovers at end of parse are top-level
     * script-mode closures, which aussom_parse_statements drains into
     * the synthetic script class. Holds (ast_funct_def *) as
     * ast_node *. */
    ast_node_array pending_closures;
    /* The scanner's per-parse state, held here only so the collector
     * can see it.
     *
     * aussom_lex_begin allocates a lex_extra with GC memory and hands
     * the pointer to flex, which stores it inside its own scanner
     * struct. flex allocates that struct with plain malloc, and the
     * collector does not trace malloc memory, so that copy of the
     * pointer keeps nothing alive. This one does: a parser_ctx is GC
     * memory and lives on parse_with_yyin's stack for the whole parse,
     * so the extra and its string buffer are reachable from a root for
     * exactly as long as the scanner is using them.
     *
     * void * rather than lex_extra *, because lex_extra is private to
     * the scanner and nothing outside it should be reading this. */
    void *lex_extra;
} parser_ctx;

parser_ctx *parser_ctx_new(engine *eng, const char *file_name);

/* Hands back the accumulated closures and resets the list. */
void parser_ctx_drain_pending_closures(parser_ctx *ctx, ast_node_array *out);

/* Semantic-action helper for top-level instructions. Appends `n` to
 * the script_stmt_target when script mode is enabled; otherwise
 * marks the parse as failed and prints a PARSE_ERROR message. */
void parser_ctx_script_or_error(parser_ctx *ctx, ast_node *n);

#endif /* AUSSOM_PARSER_CTX_H */
