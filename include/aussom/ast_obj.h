/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_OBJ_H
#define AUSSOM_AST_OBJ_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astObj. The `index` field is the optional
 * bracket-index expression: `foo[index].child`. The chain into
 * members is the inherited `child` pointer on the base. */
typedef struct ast_obj {
    ast_node base;
    ast_node *index;
} ast_obj;

ast_obj *ast_obj_new(void);

void ast_obj_set_index(ast_obj *n, ast_node *index);
ast_node *ast_obj_get_index(const ast_obj *n);

#endif /* AUSSOM_AST_OBJ_H */
