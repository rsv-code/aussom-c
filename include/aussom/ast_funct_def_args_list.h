/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_FUNCT_DEF_ARGS_LIST_H
#define AUSSOM_AST_FUNCT_DEF_ARGS_LIST_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astFunctDefArgsList. Holds the list of
 * argument AST nodes for a function definition or function call.
 * Each element is owned. */
typedef struct ast_funct_def_args_list {
    ast_node base;
    ast_node_array args;
} ast_funct_def_args_list;

ast_funct_def_args_list *ast_funct_def_args_list_new(void);
ast_funct_def_args_list *ast_funct_def_args_list_new_named(const char *name);

/* Appends an argument node. */
void ast_funct_def_args_list_add(ast_funct_def_args_list *n, ast_node *arg);

/* Read-only access to the underlying array. */
const ast_node_array *ast_funct_def_args_list_args(const ast_funct_def_args_list *n);

#endif /* AUSSOM_AST_FUNCT_DEF_ARGS_LIST_H */
