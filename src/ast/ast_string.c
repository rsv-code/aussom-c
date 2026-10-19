/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_string.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_string *ast_string_new(void)
{
    return ast_string_new_value("");
}

ast_string *ast_string_new_value(const char *value)
{
    ast_string *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_STRING;
    n->value = AUSSOM_STRDUP(value);
    return n;
}

const char *ast_string_get_value(const ast_string *n)
{
    return n->value;
}

void ast_string_set_value(ast_string *n, const char *value)
{
    n->value = AUSSOM_STRDUP(value);
}

void ast_string_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_string *s = (const ast_string *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %s\n", s->value);
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_string_eval(ast_node *n, environment *env, bool getref)
{
    return eval_child(&av_string_new(ast_string_get_value((ast_string *)n))->base.base, n, env, getref);
}
