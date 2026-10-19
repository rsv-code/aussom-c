/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_BREAK_H
#define AUSSOM_AST_BREAK_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astBreak. No payload. */
typedef struct ast_break {
    ast_node base;
} ast_break;

ast_break *ast_break_new(void);

#endif /* AUSSOM_AST_BREAK_H */
