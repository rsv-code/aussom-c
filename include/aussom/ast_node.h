/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Memory: every node, string, and array is allocated via the
 * project's GC shim (see aussom_gc.h). No destructors; unreachable
 * nodes are reclaimed by the collector. Setters that replace a
 * previously held child do so via plain assignment.
 */

#ifndef AUSSOM_AST_NODE_H
#define AUSSOM_AST_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "aussom/access_type.h"
#include "aussom/ast_node_type.h"
#include "aussom/c_type.h"

/* Forward declarations. Subtype-specific headers include this file
 * and embed `ast_node base` as their first member. */
struct ast_node;
struct ast_annotation;
struct ast_aussom_doc;

typedef struct ast_node ast_node;

/* Resizable array of ast_node *. Storage is GC-managed; do not free. */
typedef struct ast_node_array {
    ast_node **items;
    size_t count;
    size_t capacity;
} ast_node_array;

void ast_node_array_init(ast_node_array *a);
void ast_node_array_add(ast_node_array *a, ast_node *node);

/* Ordered key/value pair used by ast_map. */
typedef struct ast_node_pair {
    ast_node *key;
    ast_node *value;
} ast_node_pair;

typedef struct ast_node_pair_array {
    ast_node_pair *items;
    size_t count;
    size_t capacity;
} ast_node_pair_array;

void ast_node_pair_array_init(ast_node_pair_array *a);
void ast_node_pair_array_add(ast_node_pair_array *a, ast_node *key, ast_node *value);

/* Common fields shared by every AST node. Subtype structs put this
 * as their first member so a `subtype *` casts to `ast_node *` and
 * back without UB. Field meanings mirror com.aussom.ast.astNode. */
struct ast_node {
    char *name;
    ast_node_type type;
    char *file_name;
    int line_num;
    int col_num;
    int line_num_end;
    int col_num_end;
    access_type access;
    c_type prim_type;
    ast_node *child;
    struct ast_aussom_doc *doc_node;
    /* Array of (struct ast_annotation *) stored as ast_node *. */
    ast_node_array annotations;
    /* Debugger breakpoint flag. Mirrors the volatile field on the
     * Java side; debugger threads will mutate this while the eval
     * loop reads it. */
    volatile bool breakpoint;
};

/* Initializes the common fields on an already-allocated node. The
 * concrete-type constructors call this first, then set their type
 * and subtype-specific fields. Sets name/file_name to empty strings
 * (heap copies, never NULL), so toString can write them without
 * NULL checks. */
void ast_node_init(ast_node *n);

/* Polymorphic toString. Writes a JSON-style representation of the
 * subtree rooted at `n` to `out`. The indent level is multiplied
 * into a leading tab string, mirroring the Java getTabs() helper. */
void ast_node_to_string(const ast_node *n, int level, FILE *out);

/* Convenience wrapper: returns a freshly allocated GC-managed C
 * string holding the toString output. Returned pointer is owned by
 * the collector; do not free. */
char *ast_node_to_string_alloc(const ast_node *n);

/* Writes the "type"/"name"/"line"/"dataType"/"primativeType" lines
 * common to most toString outputs. Caller wraps with the surrounding
 * braces and any subtype-specific fields. */
void ast_node_write_node_str(const ast_node *n, int level, FILE *out);

/* Writes `level` tab characters. */
void ast_node_write_tabs(int level, FILE *out);

/* Common accessors. Setters duplicate input strings into GC memory.
 * A NULL input is treated as empty string. */
void ast_node_set_name(ast_node *n, const char *name);
void ast_node_set_file_name(ast_node *n, const char *file_name);
void ast_node_set_type(ast_node *n, ast_node_type t);

/* Appends `child` to the end of n's child chain (descending through
 * existing children). Matches astNode.appendChild. */
void ast_node_append_child(ast_node *n, ast_node *child);

/* Sets file_name + line_num + col_num. line_num_end and col_num_end
 * default to the same values. */
void ast_node_set_parser_info(ast_node *n, const char *file_name,
                              int line_num, int col_num);

/* Same, but with explicit end coordinates. */
void ast_node_set_parser_info_range(ast_node *n, const char *file_name,
                                    int line_num, int col_num,
                                    int line_num_end, int col_num_end);

#endif /* AUSSOM_AST_NODE_H */
