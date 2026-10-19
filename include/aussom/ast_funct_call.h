/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_FUNCT_CALL_H
#define AUSSOM_AST_FUNCT_CALL_H

#include "aussom/ast_funct_def_args_list.h"
#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astFunctCall. The `args` field always
 * points at an owned ast_funct_def_args_list; the constructor sets
 * up an empty one. */
typedef struct ast_funct_call {
    ast_node base;
    ast_funct_def_args_list *args;
} ast_funct_call;

ast_funct_call *ast_funct_call_new(void);
ast_funct_call *ast_funct_call_new_named(const char *name);

/* Appends a single argument expression. Convenience around
 * ast_funct_def_args_list_add. */
void ast_funct_call_add_arg(ast_funct_call *n, ast_node *arg);

void ast_funct_call_set_args(ast_funct_call *n, ast_funct_def_args_list *args);
ast_funct_def_args_list *ast_funct_call_get_args(const ast_funct_call *n);

#endif /* AUSSOM_AST_FUNCT_CALL_H */
