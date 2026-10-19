/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_throw.h"

#include "aussom/ast_dispatch.h"
#include "aussom/ast_null.h"
#include "aussom/aussom_gc.h"

ast_throw *ast_throw_new(void)
{
    ast_throw *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_THROW;
    ast_null *default_expr = ast_null_new();
    n->expression = &default_expr->base;
    return n;
}

void ast_throw_set_expression(ast_throw *n, ast_node *expression)
{
    n->expression = expression;
}

ast_node *ast_throw_get_expression(const ast_throw *n)
{
    return n->expression;
}

void ast_throw_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_throw *t = (const ast_throw *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fputs("\"expression\":\n", out);
    ast_node_to_string(t->expression, level + 2, out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- throw / try-catch ------------------------------------------- */

av_type *ast_throw_eval(ast_node *n, environment *env, bool getref)
{
    ast_throw *t = (ast_throw *)n;
    av_type *val = ast_node_eval(t->expression, env, getref);
    if (av_is_ex(val)) return val;
    char *msg = eval_str(val);
    if (msg == NULL) return eval_stack_overflow(env, n->line_num);
    av_exception *e = av_exception_new(msg);
    av_exception_set_line(e, n->line_num);
    av_exception_set_stack_trace(e, environment_stack_trace(env));
    av_exception_set_local_object(e, false);
    return &e->base.base;
}
