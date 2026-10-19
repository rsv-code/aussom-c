/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_break.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_break *ast_break_new(void)
{
    ast_break *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_BREAK;
    return n;
}

void ast_break_write_body(const ast_node *n, int level, FILE *out)
{
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputc('\n', out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_break_eval(ast_node *n, environment *env, bool getref)
{
    (void)n; (void)env; (void)getref;
    return &av_break_new()->base;
}
