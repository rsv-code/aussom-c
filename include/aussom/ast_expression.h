/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_EXPRESSION_H
#define AUSSOM_AST_EXPRESSION_H

#include "aussom/ast_node.h"
#include "aussom/exp_type.h"

/* Mirrors com.aussom.ast.astExpression. Binary operator nodes have
 * both left and right set; unary nodes (NOT, PLPL, MIMI, COUNT,
 * MISSNULL, INCLUDE) leave right NULL. */
typedef struct ast_expression {
    ast_node base;
    ast_node *left;
    ast_node *right;
    exp_type e_type;
} ast_expression;

ast_expression *ast_expression_new(void);
ast_expression *ast_expression_new_with_left(ast_node *left);

void ast_expression_set_left(ast_expression *n, ast_node *left);
void ast_expression_set_right(ast_expression *n, ast_node *right);
void ast_expression_set_etype(ast_expression *n, exp_type t);
ast_node *ast_expression_get_left(const ast_expression *n);
ast_node *ast_expression_get_right(const ast_expression *n);
exp_type ast_expression_get_etype(const ast_expression *n);

#endif /* AUSSOM_AST_EXPRESSION_H */
