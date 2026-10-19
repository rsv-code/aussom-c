/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/parser_ctx.h"

#include <stdio.h>

#include "aussom/aussom_gc.h"

parser_ctx *parser_ctx_new(engine *eng, const char *file_name)
{
    parser_ctx *ctx = AUSSOM_ALLOC(sizeof(*ctx));
    ctx->eng = eng;
    ctx->file_name = AUSSOM_STRDUP(file_name);
    ctx->load_extern_classes = false;
    ctx->script_stmt_target = NULL;
    ctx->syntax_errors = false;
    ctx->thrown = false;
    ctx->lex_extra = NULL;
    ast_node_array_init(&ctx->pending_closures);
    return ctx;
}

void parser_ctx_drain_pending_closures(parser_ctx *ctx, ast_node_array *out)
{
    *out = ctx->pending_closures;
    ast_node_array_init(&ctx->pending_closures);
}

void parser_ctx_script_or_error(parser_ctx *ctx, ast_node *n)
{
    if (ctx->script_stmt_target == NULL) {
        ctx->syntax_errors = true;
        engine_set_parse_error(ctx->eng);
        /* Parser.scriptOrError: the diagnostic carries the same
         * message the log line shows after its location. */
        static const char msg[] =
            "PARSE_ERROR: Top-level statements are not allowed in this "
            "parse context. Use Engine.setScriptMode(true) and "
            "Engine.evalLine for script-mode evaluation.";
        engine_add_parse_diagnostic(ctx->eng, ctx->file_name, n->line_num,
            n->col_num, msg);
        char text[512];
        snprintf(text, sizeof(text), "%s [%d]: %s", ctx->file_name, n->line_num, msg);
        engine_err(ctx->eng, text);
        return;
    }
    ast_statement_list_add(ctx->script_stmt_target, n);
}
