/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_CONDITION_BLOCK_H
#define AUSSOM_AST_CONDITION_BLOCK_H

#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astConditionBlock. Used inside if/elseif and
 * switch case branches: a boolean expression plus the body that
 * runs when it's true. */
typedef struct ast_condition_block {
    ast_node base;
    ast_node *expr;
    ast_statement_list *instruction_list;
} ast_condition_block;

ast_condition_block *ast_condition_block_new(void);

void ast_condition_block_set_expression(ast_condition_block *n, ast_node *expr);
ast_node *ast_condition_block_get_expression(const ast_condition_block *n);

/* Appends a statement to the body. */
void ast_condition_block_add_instruction(ast_condition_block *n, ast_node *inst);

#endif /* AUSSOM_AST_CONDITION_BLOCK_H */
