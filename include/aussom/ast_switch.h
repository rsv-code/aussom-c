/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_SWITCH_H
#define AUSSOM_AST_SWITCH_H

#include "aussom/ast_condition_block.h"
#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astSwitch. */
typedef struct ast_switch {
    ast_node base;
    ast_node *expr;
    ast_node_array case_conditions;
    ast_statement_list *default_list;
} ast_switch;

ast_switch *ast_switch_new(void);

void ast_switch_set_expression(ast_switch *n, ast_node *expr);
ast_node *ast_switch_get_expression(const ast_switch *n);

/* Appends a case branch. */
void ast_switch_add_case(ast_switch *n, ast_condition_block *cond);

/* Appends a statement to the default branch. */
void ast_switch_add_default_instruction(ast_switch *n, ast_node *inst);

#endif /* AUSSOM_AST_SWITCH_H */
