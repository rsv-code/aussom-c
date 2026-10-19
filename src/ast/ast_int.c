/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_int.h"

#include <inttypes.h>

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_int *ast_int_new(void)
{
    return ast_int_new_value(0);
}

ast_int *ast_int_new_value(int64_t value)
{
    ast_int *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_INT;
    n->value = value;
    return n;
}

int64_t ast_int_get_value(const ast_int *n)
{
    return n->value;
}

void ast_int_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_int *i = (const ast_int *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %" PRId64 "\n", i->value);
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_int_eval(ast_node *n, environment *env, bool getref)
{
    return eval_child(&av_int_new(ast_int_get_value((ast_int *)n))->base.base, n, env, getref);
}
