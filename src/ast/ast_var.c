/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_var.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_var *ast_var_new(void)
{
    ast_var *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_VAR;
    n->associative = NULL;
    return n;
}

ast_var *ast_var_new_named(const char *name)
{
    ast_var *n = ast_var_new();
    ast_node_set_name(&n->base, name);
    return n;
}

void ast_var_set_associative(ast_var *n, ast_node *associative)
{
    n->associative = associative;
}

ast_node *ast_var_get_associative(const ast_var *n)
{
    return n->associative;
}

void ast_var_write_body(const ast_node *n, int level, FILE *out)
{
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputc('\n', out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- variable ---------------------------------------------------- */

av_type *ast_var_eval(ast_node *n, environment *env)
{
    av_strmap *locals = environment_locals(env);
    if (locals != NULL) {
        av_type *v = av_strmap_get(locals, n->name);
        if (v != NULL) return v;   /* map values are never C-NULL */
    }
    return &av_null_new()->base.base;   /* load-bearing: undefined -> null */
}
