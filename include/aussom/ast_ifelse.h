/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_IFELSE_H
#define AUSSOM_AST_IFELSE_H

#include "aussom/ast_condition_block.h"
#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astIfElse. Holds the primary if condition,
 * the elseif chain (each an ast_condition_block stored as ast_node *),
 * and the final else body. */
typedef struct ast_ifelse {
    ast_node base;
    ast_condition_block *if_condition;
    ast_node_array if_else_conditions;
    ast_statement_list *else_instruction_list;
} ast_ifelse;

ast_ifelse *ast_ifelse_new(void);

void ast_ifelse_set_if_condition(ast_ifelse *n, ast_condition_block *cond);
ast_condition_block *ast_ifelse_get_if_condition(const ast_ifelse *n);

/* Appends an else-if branch. */
void ast_ifelse_add_else_if(ast_ifelse *n, ast_condition_block *cond);

/* Appends a statement to the final else body. */
void ast_ifelse_add_else_instruction(ast_ifelse *n, ast_node *inst);

#endif /* AUSSOM_AST_IFELSE_H */
