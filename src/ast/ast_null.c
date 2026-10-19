/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_null.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_null *ast_null_new(void)
{
    ast_null *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_NULL;
    return n;
}

void ast_null_write_body(const ast_node *n, int level, FILE *out)
{
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputc('\n', out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_null_eval(ast_node *n, environment *env, bool getref)
{
    return eval_child(&av_null_new()->base.base, n, env, getref);
}
