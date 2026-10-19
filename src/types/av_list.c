/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_list.h"

#include "aussom/av_bool.h"
#include "aussom/av_callback.h"
#include "aussom/av_dispatch.h"
#include "aussom/av_double.h"
#include "aussom/av_int.h"
#include "aussom/av_map.h"
#include "aussom/av_string.h"
#include "aussom/aussom_gc.h"

av_list *av_list_new(void)
{
    av_list *l = AUSSOM_ALLOC(sizeof(*l));
    av_object_init(&l->base, C_TYPE_LIST);
    l->value = av_vec_new();
    return l;
}

av_vec *av_list_vec(const av_list *l) { return l->value; }

void av_list_add(av_list *l, av_type *item) { av_vec_add(l->value, item); }
size_t av_list_size(av_list *l) { return av_vec_size(l->value); }
av_type *av_list_get(av_list *l, size_t index) { return av_vec_get(l->value, index); }
bool av_list_set(av_list *l, size_t index, av_type *item) { return av_vec_set(l->value, index, item); }
bool av_list_insert(av_list *l, size_t index, av_type *item) { return av_vec_insert(l->value, index, item); }
av_type *av_list_remove_at(av_list *l, size_t index) { return av_vec_remove_at(l->value, index); }
void av_list_clear(av_list *l) { av_vec_clear(l->value); }

/* --- typed element getters (see header) -------------------------- */

const char *av_list_get_string(av_list *l, size_t i)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL || v->type != C_TYPE_STRING) return NULL;
    return av_string_get((av_string *)v);
}

av_list *av_list_get_list(av_list *l, size_t i)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL || v->type != C_TYPE_LIST) return NULL;
    return (av_list *)v;
}

struct av_map *av_list_get_map(av_list *l, size_t i)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL || v->type != C_TYPE_MAP) return NULL;
    return (av_map *)v;
}

struct av_callback *av_list_get_callback(av_list *l, size_t i)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL || v->type != C_TYPE_CALLBACK) return NULL;
    return (av_callback *)v;
}

bool av_list_get_int(av_list *l, size_t i, int64_t *out)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL || v->type != C_TYPE_INT) return false;
    *out = av_int_get((av_int *)v);
    return true;
}

bool av_list_get_double(av_list *l, size_t i, double *out)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL) return false;
    if (v->type == C_TYPE_DOUBLE) { *out = av_double_get((av_double *)v); return true; }
    if (v->type == C_TYPE_INT) { *out = (double)av_int_get((av_int *)v); return true; } /* widen */
    return false;
}

bool av_list_get_bool(av_list *l, size_t i, bool *out)
{
    av_type *v = av_list_get(l, i);
    if (v == NULL || v->type != C_TYPE_BOOL) return false;
    *out = av_bool_get((av_bool *)v);
    return true;
}

void av_list_write(const av_type *v, int level, FILE *out)
{
    const av_list *l = (const av_list *)v;
    size_t n;
    av_type **items = av_vec_snapshot(l->value, &n);
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cList\",\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"length\": %zu,\n", n);
    av_write_tabs(level + 1, out);
    fputs("\"values\": [\n", out);
    for (size_t i = 0; i < n; i++) {
        av_to_string(items[i], level + 2, out);
        fputs(",\n", out);
    }
    av_write_tabs(level + 1, out);
    fputs("]\n", out);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_list_write_str(const av_type *v, int level, FILE *out)
{
    const av_list *l = (const av_list *)v;
    size_t n;
    av_type **items = av_vec_snapshot(l->value, &n);
    if (n > 0) {
        fputs("[\n", out);
        for (size_t i = 0; i < n; i++) {
            av_write_tabs(level + 1, out);
            av_write_str(items[i], level + 1, out);
            if (i + 1 < n) fputc(',', out);
            fputc('\n', out);
        }
        av_write_tabs(level, out);
        fputc(']', out);
    } else {
        fputs("[]", out);
    }
}

av_type *av_list_clone(const av_type *v)
{
    const av_list *l = (const av_list *)v;
    av_list *n = av_list_new();
    size_t count;
    av_type **items = av_vec_snapshot(l->value, &count);
    for (size_t i = 0; i < count; i++) {
        av_list_add(n, av_clone(items[i]));
    }
    return &n->base.base;
}
