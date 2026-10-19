/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_VAR_H
#define AUSSOM_AST_VAR_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astVar. */
typedef struct ast_var {
    ast_node base;
    ast_node *associative;
} ast_var;

ast_var *ast_var_new(void);
ast_var *ast_var_new_named(const char *name);

void ast_var_set_associative(ast_var *n, ast_node *associative);
ast_node *ast_var_get_associative(const ast_var *n);

#endif /* AUSSOM_AST_VAR_H */
