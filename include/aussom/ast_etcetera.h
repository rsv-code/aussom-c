/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_ETCETERA_H
#define AUSSOM_AST_ETCETERA_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astEtcetera. In Java this extends astVar;
 * here it's a separate struct with the same shape. The primitive
 * type tag on the base records the element type (e.g. "...int"). */
typedef struct ast_etcetera {
    ast_node base;
    ast_node *associative;
} ast_etcetera;

ast_etcetera *ast_etcetera_new(void);

#endif /* AUSSOM_AST_ETCETERA_H */
