/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_WHILE_H
#define AUSSOM_AST_WHILE_H

#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astWhile. */
typedef struct ast_while {
    ast_node base;
    ast_node *expr;
    ast_statement_list *instructions;
} ast_while;

ast_while *ast_while_new(void);

void ast_while_set_expr(ast_while *n, ast_node *expr);
ast_node *ast_while_get_expr(const ast_while *n);

/* Appends a statement to the body. */
void ast_while_add_instruction(ast_while *n, ast_node *inst);

#endif /* AUSSOM_AST_WHILE_H */
