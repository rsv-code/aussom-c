/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_FOR_H
#define AUSSOM_AST_FOR_H

#include <stdbool.h>

#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astFor. Supports both classic C-style for
 * (expr_init / expr_cond / expr_inc) and for-each
 * (each_var in each_expr). is_for_each selects between the two
 * shapes; the unused fields stay NULL. */
typedef struct ast_for {
    ast_node base;
    ast_node *expr_init;
    ast_node *expr_cond;
    ast_node *expr_inc;
    ast_node *each_var;
    ast_node *each_expr;
    bool is_for_each;
    ast_statement_list *instructions;
} ast_for;

ast_for *ast_for_new(void);

void ast_for_set_expr_init(ast_for *n, ast_node *expr);
void ast_for_set_expr_cond(ast_for *n, ast_node *expr);
void ast_for_set_expr_inc(ast_for *n, ast_node *expr);
void ast_for_set_each_var(ast_for *n, ast_node *var);
void ast_for_set_each_expr(ast_for *n, ast_node *expr);
void ast_for_set_is_for_each(ast_for *n, bool is_for_each);

/* Appends a statement to the body. */
void ast_for_add_instruction(ast_for *n, ast_node *inst);

#endif /* AUSSOM_AST_FOR_H */
