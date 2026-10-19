/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_ANNOTATION_ARG_H
#define AUSSOM_AST_ANNOTATION_ARG_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astAnnotationArg. A single name/value pair
 * inside an @Annotation declaration. */
typedef struct ast_annotation_arg {
    ast_node base;
    char *key;
    char *value;
} ast_annotation_arg;

ast_annotation_arg *ast_annotation_arg_new(void);
ast_annotation_arg *ast_annotation_arg_new_pair(const char *key, const char *value);

void ast_annotation_arg_set_key(ast_annotation_arg *n, const char *key);
const char *ast_annotation_arg_get_key(const ast_annotation_arg *n);
void ast_annotation_arg_set_value(ast_annotation_arg *n, const char *value);
const char *ast_annotation_arg_get_value(const ast_annotation_arg *n);

#endif /* AUSSOM_AST_ANNOTATION_ARG_H */
