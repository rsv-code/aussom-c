/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_ANNOTATION_H
#define AUSSOM_AST_ANNOTATION_H

#include "aussom/ast_annotation_arg.h"
#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astAnnotation. The args array stores
 * (ast_annotation_arg *) values cast to ast_node *, dispatched via
 * AST_TYPE_ANNOTATION_ARG. */
typedef struct ast_annotation {
    ast_node base;
    char *annotation_name;
    ast_node_array args;
} ast_annotation;

ast_annotation *ast_annotation_new(void);
ast_annotation *ast_annotation_new_named(const char *annotation_name);

void ast_annotation_set_name(ast_annotation *n, const char *annotation_name);
const char *ast_annotation_get_name(const ast_annotation *n);

/* Appends a named-arg pair. */
void ast_annotation_add_arg(ast_annotation *n, ast_annotation_arg *arg);

/* Returns the first arg whose key equals `key`, or NULL. The
 * pointer references storage owned by the annotation. */
ast_annotation_arg *ast_annotation_find_arg(const ast_annotation *n, const char *key);

/* Returns the value of the first arg with the matching key, or NULL
 * when no such arg exists. */
const char *ast_annotation_find_value(const ast_annotation *n, const char *key);

#endif /* AUSSOM_AST_ANNOTATION_H */
