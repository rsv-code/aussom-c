/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_STRING_H
#define AUSSOM_AST_STRING_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astString. Owns its value buffer. */
typedef struct ast_string {
    ast_node base;
    char *value;
} ast_string;

ast_string *ast_string_new(void);
ast_string *ast_string_new_value(const char *value);
const char *ast_string_get_value(const ast_string *n);

/* Replaces the stored value. The input is copied; the node retains
 * ownership of the copy. */
void ast_string_set_value(ast_string *n, const char *value);

#endif /* AUSSOM_AST_STRING_H */
