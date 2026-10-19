/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_BOOL_H
#define AUSSOM_AST_BOOL_H

#include <stdbool.h>

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astBool. */
typedef struct ast_bool {
    ast_node base;
    bool value;
} ast_bool;

ast_bool *ast_bool_new(void);
ast_bool *ast_bool_new_value(bool value);
bool ast_bool_get_value(const ast_bool *n);

#endif /* AUSSOM_AST_BOOL_H */
