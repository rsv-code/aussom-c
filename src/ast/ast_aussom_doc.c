/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_aussom_doc.h"

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

static char *substr_trim(const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - start);
    char *out = AUSSOM_ALLOC_ATOMIC(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static bool line_is_annotation(const char *line)
{
    if (line[0] != '@') return false;
    if (!(isalnum((unsigned char)line[1]) || line[1] == '_')) return false;
    return true;
}

static void doc_list_grow(ast_aussom_doc *d)
{
    if (d->doc_list_count < d->doc_list_capacity) return;
    size_t new_cap = (d->doc_list_capacity == 0) ? 4 : d->doc_list_capacity * 2;
    d->doc_list = AUSSOM_REALLOC(d->doc_list, new_cap * sizeof(*d->doc_list));
    d->doc_list_capacity = new_cap;
}

/* Strips a leading run of '*' chars from each line plus surrounding
 * whitespace, drops empty lines, rejoins. Result replaces the input
 * text on the node. */
static void strip_formatting(ast_aussom_doc *d)
{
    size_t cap = strlen(d->aussom_doc_text) + 1;
    char *out = AUSSOM_ALLOC_ATOMIC(cap);
    out[0] = '\0';
    size_t out_len = 0;

    const char *p = d->aussom_doc_text;
    while (*p != '\0') {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') p++;
        const char *line_end = p;
        char *line = substr_trim(line_start, line_end);
        char *trimmed = line;
        if (trimmed[0] == '*') {
            while (*trimmed == '*') trimmed++;
            while (*trimmed != '\0' && isspace((unsigned char)*trimmed)) trimmed++;
            size_t tlen = strlen(trimmed);
            while (tlen > 0 && isspace((unsigned char)trimmed[tlen - 1])) {
                trimmed[--tlen] = '\0';
            }
        }
        if (trimmed[0] != '\0') {
            size_t tlen = strlen(trimmed);
            if (out_len + tlen + 2 > cap) {
                size_t new_cap = (out_len + tlen + 2) * 2;
                out = AUSSOM_REALLOC(out, new_cap);
                cap = new_cap;
            }
            memcpy(out + out_len, trimmed, tlen);
            out_len += tlen;
            out[out_len++] = '\n';
            out[out_len] = '\0';
        }
        if (*p == '\n') p++;
    }
    /* Trim trailing whitespace. */
    while (out_len > 0 && isspace((unsigned char)out[out_len - 1])) {
        out[--out_len] = '\0';
    }
    d->aussom_doc_text = out;
}

/* Groups consecutive non-blank, non-annotation lines into one
 * paragraph; emits each annotation line as its own paragraph.
 * Returns a GC-allocated array of GC-allocated strings; *count
 * receives the entry count. */
static char **process_lines(const char *text, size_t *count)
{
    *count = 0;
    char **lines = NULL;
    size_t capacity = 0;
    char *cur = NULL;
    size_t cur_len = 0;
    size_t cur_cap = 0;

    const char *p = text;
    while (*p != '\0') {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') p++;
        const char *line_end = p;
        /* Skip leading whitespace for the annotation check. */
        const char *check = line_start;
        while (check < line_end && isspace((unsigned char)*check)) check++;
        bool is_annotation = (check < line_end) && (*check == '@')
            && (check + 1 < line_end)
            && (isalnum((unsigned char)*(check + 1)) || *(check + 1) == '_');
        bool is_blank = (check == line_end);

        if (is_annotation || is_blank) {
            /* Flush accumulated paragraph. */
            if (cur != NULL && cur_len > 0) {
                while (cur_len > 0 && isspace((unsigned char)cur[cur_len - 1])) {
                    cur[--cur_len] = '\0';
                }
                if (cur_len > 0) {
                    if (*count == capacity) {
                        size_t nc = (capacity == 0) ? 4 : capacity * 2;
                        lines = AUSSOM_REALLOC(lines, nc * sizeof(*lines));
                        capacity = nc;
                    }
                    lines[(*count)++] = cur;
                }
                cur = NULL;
                cur_len = 0;
                cur_cap = 0;
            }
        }
        if (is_annotation || (!is_blank && !is_annotation)) {
            size_t add_len = (size_t)(line_end - line_start) + 1;
            if (cur_len + add_len + 1 > cur_cap) {
                size_t nc = (cur_cap == 0) ? 64 : cur_cap * 2;
                while (cur_len + add_len + 1 > nc) nc *= 2;
                cur = AUSSOM_REALLOC(cur, nc);
                cur_cap = nc;
            }
            memcpy(cur + cur_len, line_start, (size_t)(line_end - line_start));
            cur_len += (size_t)(line_end - line_start);
            cur[cur_len++] = ' ';
            cur[cur_len] = '\0';
        }
        if (*p == '\n') p++;
    }
    /* Flush trailing accumulated paragraph. */
    if (cur != NULL && cur_len > 0) {
        while (cur_len > 0 && isspace((unsigned char)cur[cur_len - 1])) {
            cur[--cur_len] = '\0';
        }
        if (cur_len > 0) {
            if (*count == capacity) {
                size_t nc = (capacity == 0) ? 4 : capacity * 2;
                lines = AUSSOM_REALLOC(lines, nc * sizeof(*lines));
                capacity = nc;
            }
            lines[(*count)++] = cur;
        }
    }
    return lines;
}

static void parse_text(ast_aussom_doc *d)
{
    size_t line_count = 0;
    char **lines = process_lines(d->aussom_doc_text, &line_count);
    for (size_t i = 0; i < line_count; i++) {
        char *line = lines[i];
        if (line_is_annotation(line)) {
            doc_text *an = doc_annotation_new(line);
            /* Split on whitespace: parts[0] is the @tag, parts[1] is
             * the value, the rest is the description. */
            const char *p = line + 1; /* skip leading '@' */
            while (*p != '\0' && !isspace((unsigned char)*p)) p++;
            char *tag = substr_trim(line + 1, p);
            doc_text_set_tag_name(an, tag);
            /* Skip whitespace to value start. */
            while (*p != '\0' && isspace((unsigned char)*p)) p++;
            const char *value_start = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) p++;
            const char *value_end = p;
            if (value_start < value_end) {
                char *val = substr_trim(value_start, value_end);
                doc_text_set_value(an, val);
            }
            /* Skip whitespace to description start. */
            while (*p != '\0' && isspace((unsigned char)*p)) p++;
            const char *desc_start = p;
            const char *desc_end = line + strlen(line);
            if (desc_start < desc_end) {
                char *desc = substr_trim(desc_start, desc_end);
                doc_text_set_description(an, desc);
            }
            doc_list_grow(d);
            d->doc_list[d->doc_list_count++] = an;
        } else {
            doc_text *t = doc_text_new(line);
            doc_list_grow(d);
            d->doc_list[d->doc_list_count++] = t;
        }
    }
}

ast_aussom_doc *ast_aussom_doc_new(void)
{
    ast_aussom_doc *d = AUSSOM_ALLOC(sizeof(*d));
    ast_node_init(&d->base);
    d->base.type = AST_TYPE_AUSSOM_DOC;
    d->aussom_doc_text = AUSSOM_STRDUP("");
    d->parsed = false;
    d->doc_list = NULL;
    d->doc_list_count = 0;
    d->doc_list_capacity = 0;
    return d;
}

/* Runs the strip and parse, once, on first read. Not synchronized: an
 * AST is built by one thread and read-only afterwards, and the read
 * paths that reach here (doc generation, reflect) run on the
 * interpreter thread that owns it. If a doc node ever becomes readable
 * from an embedder thread while another parses, this needs a lock. */
static void ensure_parsed(ast_aussom_doc *d)
{
    if (d->parsed) return;
    d->parsed = true;
    strip_formatting(d);
    parse_text(d);
}

const char *ast_aussom_doc_get_text(ast_aussom_doc *d)
{
    ensure_parsed(d);
    return d->aussom_doc_text;
}

size_t ast_aussom_doc_entry_count(ast_aussom_doc *d)
{
    ensure_parsed(d);
    return d->doc_list_count;
}

doc_text *ast_aussom_doc_entry_at(ast_aussom_doc *d, size_t i)
{
    ensure_parsed(d);
    if (i >= d->doc_list_count) return NULL;
    return d->doc_list[i];
}

ast_aussom_doc *ast_aussom_doc_new_dropped(void)
{
    ast_aussom_doc *d = ast_aussom_doc_new();
    d->aussom_doc_text = AUSSOM_STRDUP("");
    /* Nothing to strip or split, so the deferred work is already done. */
    d->parsed = true;
    return d;
}

ast_aussom_doc *ast_aussom_doc_new_text(const char *text)
{
    ast_aussom_doc *d = ast_aussom_doc_new();
    d->aussom_doc_text = AUSSOM_STRDUP(text);
    d->parsed = false;
    return d;
}

void ast_aussom_doc_write_body(const ast_node *n, int level, FILE *out)
{
    /* Casting away const to trigger the deferred parse: the node is
     * logically immutable, and filling in the parsed form is caching
     * rather than mutation. */
    ast_aussom_doc *d = (ast_aussom_doc *)(uintptr_t)(const void *)n;
    ensure_parsed(d);
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"fileName\": \"%s\",\n", n->file_name);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"aussomDocText\": \"%s\",\n", d->aussom_doc_text);
}
