/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_THROW_H
#define AUSSOM_AST_THROW_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astThrow. The constructor installs an
 * ast_null as the default expression, matching the Java side. */
typedef struct ast_throw {
    ast_node base;
    ast_node *expression;
} ast_throw;

ast_throw *ast_throw_new(void);

void ast_throw_set_expression(ast_throw *n, ast_node *expression);
ast_node *ast_throw_get_expression(const ast_throw *n);

#endif /* AUSSOM_AST_THROW_H */
