/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_annotation_arg.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_annotation_arg *ast_annotation_arg_new(void)
{
    return ast_annotation_arg_new_pair("", "");
}

ast_annotation_arg *ast_annotation_arg_new_pair(const char *key, const char *value)
{
    ast_annotation_arg *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_ANNOTATION_ARG;
    n->key = AUSSOM_STRDUP(key);
    n->value = AUSSOM_STRDUP(value);
    return n;
}

void ast_annotation_arg_set_key(ast_annotation_arg *n, const char *key)
{
    n->key = AUSSOM_STRDUP(key);
}

const char *ast_annotation_arg_get_key(const ast_annotation_arg *n)
{
    return n->key;
}

void ast_annotation_arg_set_value(ast_annotation_arg *n, const char *value)
{
    n->value = AUSSOM_STRDUP(value);
}

const char *ast_annotation_arg_get_value(const ast_annotation_arg *n)
{
    return n->value;
}

void ast_annotation_arg_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_annotation_arg *a = (const ast_annotation_arg *)n;
    ast_node_write_tabs(level, out);
    fprintf(out, "{ %s: \"%s\" }\n", a->key, a->value);
}
