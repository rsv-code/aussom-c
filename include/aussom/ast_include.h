/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_INCLUDE_H
#define AUSSOM_AST_INCLUDE_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astInclude. Stores the dotted include path
 * as an ordered list of name parts: `pkg.mod.file` parses to
 * ["pkg", "mod", "file"]. Helpers reconstruct the filesystem path
 * (parts joined with '/' + ".aus") or the extern class name (parts
 * joined with '.'). */
typedef struct ast_include {
    ast_node base;
    char **parts;
    size_t parts_count;
    size_t parts_capacity;
    char *ext;
} ast_include;

ast_include *ast_include_new(void);
ast_include *ast_include_new_named(const char *name);

/* Adds a single path part. The string is duplicated into GC memory. */
void ast_include_add_name(ast_include *n, const char *name);

/* Convenience: splits a dot-delimited string and appends every part. */
void ast_include_add_full_string(ast_include *n, const char *full_string);

/* Returns a GC-managed string holding the filesystem path (parts
 * joined with '/' + ".aus" extension). */
char *ast_include_get_path(const ast_include *n);

/* Returns a GC-managed string holding the parts joined with '.',
 * no extension. */
char *ast_include_get_extern_class(const ast_include *n);

#endif /* AUSSOM_AST_INCLUDE_H */
