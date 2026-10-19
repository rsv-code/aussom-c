/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_RETURN_H
#define AUSSOM_AST_RETURN_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astReturn. Optional return value expression. */
typedef struct ast_return {
    ast_node base;
    ast_node *value;
} ast_return;

ast_return *ast_return_new(void);

void ast_return_set_value(ast_return *n, ast_node *value);
ast_node *ast_return_get_value(const ast_return *n);

#endif /* AUSSOM_AST_RETURN_H */
