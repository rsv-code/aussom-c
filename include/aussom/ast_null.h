/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_NULL_H
#define AUSSOM_AST_NULL_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astNull. Literal null value with no payload. */
typedef struct ast_null {
    ast_node base;
} ast_null;

ast_null *ast_null_new(void);

#endif /* AUSSOM_AST_NULL_H */
