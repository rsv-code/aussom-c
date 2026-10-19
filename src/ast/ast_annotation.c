/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_annotation.h"

#include <string.h>

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_annotation *ast_annotation_new(void)
{
    return ast_annotation_new_named("");
}

ast_annotation *ast_annotation_new_named(const char *annotation_name)
{
    ast_annotation *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_ANNOTATION;
    n->annotation_name = AUSSOM_STRDUP(annotation_name);
    ast_node_array_init(&n->args);
    return n;
}

void ast_annotation_set_name(ast_annotation *n, const char *annotation_name)
{
    n->annotation_name = AUSSOM_STRDUP(annotation_name);
}

const char *ast_annotation_get_name(const ast_annotation *n)
{
    return n->annotation_name;
}

void ast_annotation_add_arg(ast_annotation *n, ast_annotation_arg *arg)
{
    ast_node_array_add(&n->args, &arg->base);
}

ast_annotation_arg *ast_annotation_find_arg(const ast_annotation *n, const char *key)
{
    if (key == NULL) return NULL;
    for (size_t i = 0; i < n->args.count; i++) {
        ast_annotation_arg *a = (ast_annotation_arg *)n->args.items[i];
        if (strcmp(ast_annotation_arg_get_key(a), key) == 0) return a;
    }
    return NULL;
}

const char *ast_annotation_find_value(const ast_annotation *n, const char *key)
{
    ast_annotation_arg *a = ast_annotation_find_arg(n, key);
    return (a != NULL) ? ast_annotation_arg_get_value(a) : NULL;
}

void ast_annotation_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_annotation *a = (const ast_annotation *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"name\": \"%s\",\n", a->annotation_name);
    ast_node_write_tabs(level + 1, out);
    fputs("\"args\": {\n", out);
    for (size_t i = 0; i < a->args.count; i++) {
        ast_node_to_string(a->args.items[i], level + 2, out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("}\n", out);
}
