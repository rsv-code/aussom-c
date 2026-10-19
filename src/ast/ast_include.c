/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_include.h"

#include <string.h>

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

static void parts_grow(ast_include *n)
{
    if (n->parts_count < n->parts_capacity) return;
    size_t new_cap = (n->parts_capacity == 0) ? 4 : n->parts_capacity * 2;
    n->parts = AUSSOM_REALLOC(n->parts, new_cap * sizeof(*n->parts));
    n->parts_capacity = new_cap;
}

ast_include *ast_include_new(void)
{
    ast_include *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_INCLUDE;
    n->parts = NULL;
    n->parts_count = 0;
    n->parts_capacity = 0;
    n->ext = AUSSOM_STRDUP(".aus");
    return n;
}

ast_include *ast_include_new_named(const char *name)
{
    ast_include *n = ast_include_new();
    ast_include_add_name(n, name);
    return n;
}

void ast_include_add_name(ast_include *n, const char *name)
{
    parts_grow(n);
    n->parts[n->parts_count++] = AUSSOM_STRDUP(name);
}

void ast_include_add_full_string(ast_include *n, const char *full_string)
{
    if (full_string == NULL) return;
    /* Clear existing parts; the Java side replaces the list. */
    n->parts_count = 0;

    const char *start = full_string;
    const char *p = full_string;
    while (*p != '\0') {
        if (*p == '.') {
            size_t len = (size_t)(p - start);
            char *part = AUSSOM_ALLOC_ATOMIC(len + 1);
            memcpy(part, start, len);
            part[len] = '\0';
            parts_grow(n);
            n->parts[n->parts_count++] = part;
            start = p + 1;
        }
        p++;
    }
    /* Trailing part (or whole string if no '.'). */
    size_t len = (size_t)(p - start);
    char *part = AUSSOM_ALLOC_ATOMIC(len + 1);
    memcpy(part, start, len);
    part[len] = '\0';
    parts_grow(n);
    n->parts[n->parts_count++] = part;
}

static char *join_parts(const ast_include *n, char sep, const char *trailing)
{
    size_t total = 0;
    for (size_t i = 0; i < n->parts_count; i++) {
        total += strlen(n->parts[i]);
        if (i + 1 < n->parts_count) total += 1;
    }
    if (n->parts_count > 0 && trailing != NULL) total += strlen(trailing);
    char *out = AUSSOM_ALLOC_ATOMIC(total + 1);
    size_t off = 0;
    for (size_t i = 0; i < n->parts_count; i++) {
        size_t len = strlen(n->parts[i]);
        memcpy(out + off, n->parts[i], len);
        off += len;
        if (i + 1 < n->parts_count) {
            out[off++] = sep;
        }
    }
    if (n->parts_count > 0 && trailing != NULL) {
        size_t len = strlen(trailing);
        memcpy(out + off, trailing, len);
        off += len;
    }
    out[off] = '\0';
    return out;
}

char *ast_include_get_path(const ast_include *n)
{
    return join_parts(n, '/', n->ext);
}

char *ast_include_get_extern_class(const ast_include *n)
{
    return join_parts(n, '.', NULL);
}

void ast_include_write_body(const ast_node *n, int level, FILE *out)
{
    /* The Java toString returns null in astInclude; emit a minimal
     * brace-pair so dumps don't break. */
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputc('\n', out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_include_eval(ast_node *n, environment *env, bool getref)
{
    (void)env; (void)getref;
    char *path = ast_include_get_path((ast_include *)n);
    return &av_string_new(path)->base.base;
}
