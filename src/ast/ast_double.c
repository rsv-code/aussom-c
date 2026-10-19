/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_double.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_double *ast_double_new(void)
{
    return ast_double_new_value(0.0);
}

ast_double *ast_double_new_value(double value)
{
    ast_double *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_DOUBLE;
    n->value = value;
    return n;
}

double ast_double_get_value(const ast_double *n)
{
    return n->value;
}

void ast_double_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_double *d = (const ast_double *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %g\n", d->value);
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_double_eval(ast_node *n, environment *env, bool getref)
{
    return eval_child(&av_double_new(ast_double_get_value((ast_double *)n))->base.base, n, env, getref);
}
