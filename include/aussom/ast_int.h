/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_INT_H
#define AUSSOM_AST_INT_H

#include <stdint.h>

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astInt. Stores a signed 64-bit integer to
 * match Java's long. */
typedef struct ast_int {
    ast_node base;
    int64_t value;
} ast_int;

ast_int *ast_int_new(void);
ast_int *ast_int_new_value(int64_t value);
int64_t ast_int_get_value(const ast_int *n);

#endif /* AUSSOM_AST_INT_H */
