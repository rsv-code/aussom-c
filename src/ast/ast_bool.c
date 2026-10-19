/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_bool.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_bool *ast_bool_new(void)
{
    return ast_bool_new_value(false);
}

ast_bool *ast_bool_new_value(bool value)
{
    ast_bool *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_BOOL;
    n->value = value;
    return n;
}

bool ast_bool_get_value(const ast_bool *n)
{
    return n->value;
}

void ast_bool_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_bool *b = (const ast_bool *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %s\n", b->value ? "true" : "false");
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_bool_eval(ast_node *n, environment *env, bool getref)
{
    return eval_child(&av_bool_new(ast_bool_get_value((ast_bool *)n))->base.base, n, env, getref);
}
