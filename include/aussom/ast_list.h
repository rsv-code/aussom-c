/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_LIST_H
#define AUSSOM_AST_LIST_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astList. Items are owned. */
typedef struct ast_list {
    ast_node base;
    ast_node_array items;
} ast_list;

ast_list *ast_list_new(void);

/* Appends an item to the list. The list takes ownership of the
 * node via the GC's reachability tracking. */
void ast_list_add(ast_list *n, ast_node *item);

size_t ast_list_size(const ast_list *n);

#endif /* AUSSOM_AST_LIST_H */
