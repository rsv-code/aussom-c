/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_MAP_H
#define AUSSOM_AST_MAP_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astMap. Pairs preserve insertion order;
 * Java uses ConcurrentHashMap, but iteration order isn't meaningful
 * for an AST node. Both key and value AST nodes are owned. */
typedef struct ast_map {
    ast_node base;
    ast_node_pair_array pairs;
} ast_map;

ast_map *ast_map_new(void);

/* Adds a key/value pair. The map references both nodes via GC. */
void ast_map_add(ast_map *n, ast_node *key, ast_node *value);

size_t ast_map_size(const ast_map *n);

#endif /* AUSSOM_AST_MAP_H */
