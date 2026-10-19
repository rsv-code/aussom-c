/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_map.h"

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_map *av_map_new(void)
{
    av_map *m = AUSSOM_ALLOC(sizeof(*m));
    av_object_init(&m->base, C_TYPE_MAP);
    m->value = av_strmap_new();
    return m;
}

av_strmap *av_map_strmap(const av_map *m) { return m->value; }

void av_map_put(av_map *m, const char *key, av_type *value) { av_strmap_put(m->value, key, value); }
av_type *av_map_get(av_map *m, const char *key) { return av_strmap_get(m->value, key); }
bool av_map_contains(av_map *m, const char *key) { return av_strmap_contains(m->value, key); }
av_type *av_map_remove(av_map *m, const char *key) { return av_strmap_remove(m->value, key); }
size_t av_map_size(av_map *m) { return av_strmap_size(m->value); }
void av_map_clear(av_map *m) { av_strmap_clear(m->value); }

void av_map_write(const av_type *v, int level, FILE *out)
{
    const av_map *m = (const av_map *)v;
    const char **keys;
    av_type **vals;
    size_t n = av_strmap_snapshot(m->value, &keys, &vals);
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cMap\",\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"values\": {\n", out);
    for (size_t i = 0; i < n; i++) {
        av_write_tabs(level + 2, out);
        fprintf(out, "%s:\n", keys[i]);
        av_to_string(vals[i], level + 2, out);
        fputs(",\n", out);
    }
    av_write_tabs(level + 1, out);
    fputs("}\n", out);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_map_write_str(const av_type *v, int level, FILE *out)
{
    const av_map *m = (const av_map *)v;
    const char **keys;
    av_type **vals;
    size_t n = av_strmap_snapshot(m->value, &keys, &vals);
    if (n > 0) {
        fputs("{\n", out);
        for (size_t i = 0; i < n; i++) {
            av_write_tabs(level + 1, out);
            fprintf(out, "\"%s\": ", keys[i]);
            av_write_str(vals[i], level + 1, out);
            if (i + 1 < n) fputc(',', out);
            fputc('\n', out);
        }
        av_write_tabs(level, out);
        fputc('}', out);
    } else {
        fputs("{}", out);
    }
}

av_type *av_map_clone(const av_type *v)
{
    const av_map *m = (const av_map *)v;
    av_map *n = av_map_new();
    const char **keys;
    av_type **vals;
    size_t count = av_strmap_snapshot(m->value, &keys, &vals);
    for (size_t i = 0; i < count; i++) {
        av_map_put(n, keys[i], av_clone(vals[i]));
    }
    return &n->base.base;
}
