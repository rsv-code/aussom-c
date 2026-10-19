/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_return.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_return *ast_return_new(void)
{
    ast_return *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_RETURN;
    n->value = NULL;
    return n;
}

void ast_return_set_value(ast_return *n, ast_node *value)
{
    n->value = value;
}

ast_node *ast_return_get_value(const ast_return *n)
{
    return n->value;
}

void ast_return_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_return *r = (const ast_return *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (r->value != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"value\": [\n", out);
        ast_node_to_string(r->value, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- return / break ---------------------------------------------- */

av_type *ast_return_eval(ast_node *n, environment *env, bool getref)
{
    ast_return *r = (ast_return *)n;
    av_type *val;
    if (r->value != NULL) {
        val = ast_node_eval(r->value, env, getref);
        if (av_is_ex(val)) return val;
    } else {
        val = &av_null_new()->base.base;
    }
    return &av_return_new(val)->base;
}
