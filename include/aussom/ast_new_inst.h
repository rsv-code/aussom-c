/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_NEW_INST_H
#define AUSSOM_AST_NEW_INST_H

#include "aussom/ast_funct_def_args_list.h"
#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astNewInst. The class name is stored on the
 * base's `name` field; the constructor argument list lives in
 * `args`. */
typedef struct ast_new_inst {
    ast_node base;
    ast_funct_def_args_list *args;
} ast_new_inst;

ast_new_inst *ast_new_inst_new(void);
ast_new_inst *ast_new_inst_new_named(const char *name);

void ast_new_inst_add_arg(ast_new_inst *n, ast_node *arg);

void ast_new_inst_set_args(ast_new_inst *n, ast_funct_def_args_list *args);
ast_funct_def_args_list *ast_new_inst_get_args(const ast_new_inst *n);

#endif /* AUSSOM_AST_NEW_INST_H */
