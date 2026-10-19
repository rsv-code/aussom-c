/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_DOUBLE_H
#define AUSSOM_AST_DOUBLE_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astDouble. */
typedef struct ast_double {
    ast_node base;
    double value;
} ast_double;

ast_double *ast_double_new(void);
ast_double *ast_double_new_value(double value);
double ast_double_get_value(const ast_double *n);

#endif /* AUSSOM_AST_DOUBLE_H */
