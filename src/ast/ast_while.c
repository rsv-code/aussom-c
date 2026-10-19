/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_while.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_while *ast_while_new(void)
{
    ast_while *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_WHILE;
    n->expr = NULL;
    n->instructions = ast_statement_list_new();
    return n;
}

void ast_while_set_expr(ast_while *n, ast_node *expr)
{
    n->expr = expr;
}

ast_node *ast_while_get_expr(const ast_while *n)
{
    return n->expr;
}

void ast_while_add_instruction(ast_while *n, ast_node *inst)
{
    ast_statement_list_add(n->instructions, inst);
}

void ast_while_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_while *w = (const ast_while *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (w->expr != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"condition\":\n", out);
        ast_node_to_string(w->expr, level + 1, out);
        fputs(",\n", out);
    }
    if (w->instructions != NULL) {
        ast_node_to_string(&w->instructions->base, level + 1, out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- while ------------------------------------------------------- */

av_type *ast_while_eval(ast_node *n, environment *env, bool getref)
{
    ast_while *w = (ast_while *)n;
    av_type *ret = &av_null_new()->base.base;
    bool cond = false;
    av_type *sig = eval_condition(w->expr, env, getref, &cond);
    if (sig != NULL) return sig;
    while (cond) {
        /* Cancellation check on the back edge. See the Cancellation
         * section in engine.h. */
        av_type *cancelled = eval_check_control(n, env);
        if (cancelled != NULL) return cancelled;

        for (size_t i = 0; i < w->instructions->statements.count; i++) {
            ret = ast_node_eval(w->instructions->statements.items[i], env, getref);
            if (av_is_break_return_except(ret)) break;
        }
        if (av_is_break_return_except(ret)) {
            if (!av_is_ex(ret) && av_is_break_event(ret)) ret = &av_null_new()->base.base;
            break;
        }
        sig = eval_condition(w->expr, env, getref, &cond);
        if (sig != NULL) return sig;
    }
    return ret;
}
