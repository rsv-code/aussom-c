/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_TRYCATCH_H
#define AUSSOM_AST_TRYCATCH_H

#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astTryCatch. The catch variable name is
 * stored on the base node's `name` field, matching the Java side. */
typedef struct ast_trycatch {
    ast_node base;
    ast_statement_list *try_inst_list;
    ast_statement_list *catch_inst_list;
} ast_trycatch;

ast_trycatch *ast_trycatch_new(void);

void ast_trycatch_set_try_inst_list(ast_trycatch *n, ast_statement_list *list);
ast_statement_list *ast_trycatch_get_try_inst_list(const ast_trycatch *n);

void ast_trycatch_set_catch_inst_list(ast_trycatch *n, ast_statement_list *list);
ast_statement_list *ast_trycatch_get_catch_inst_list(const ast_trycatch *n);

#endif /* AUSSOM_AST_TRYCATCH_H */
