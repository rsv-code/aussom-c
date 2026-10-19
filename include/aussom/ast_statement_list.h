/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_STATEMENT_LIST_H
#define AUSSOM_AST_STATEMENT_LIST_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astStatementList. Holds a sequence of
 * statement ast_node pointers, all owned. */
typedef struct ast_statement_list {
    ast_node base;
    ast_node_array statements;
} ast_statement_list;

ast_statement_list *ast_statement_list_new(void);
ast_statement_list *ast_statement_list_new_named(const char *name);

/* Appends a statement. The list references the node via GC. */
void ast_statement_list_add(ast_statement_list *n, ast_node *stmt);

#endif /* AUSSOM_AST_STATEMENT_LIST_H */
