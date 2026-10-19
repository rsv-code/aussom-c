/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_int.h"

#include <inttypes.h>

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_int *av_int_new(int64_t value)
{
    av_int *i = AUSSOM_ALLOC(sizeof(*i));
    av_object_init(&i->base, C_TYPE_INT);
    i->value = value;
    return i;
}

int64_t av_int_get(const av_int *i) { return i->value; }
void av_int_set(av_int *i, int64_t value) { i->value = value; }

void av_int_write(const av_type *v, int level, FILE *out)
{
    const av_int *i = (const av_int *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cInt\",\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %" PRId64 "\n", i->value);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_int_write_str(const av_type *v, int level, FILE *out)
{
    (void)level;
    const av_int *i = (const av_int *)v;
    fprintf(out, "%" PRId64, i->value);
}

av_type *av_int_clone(const av_type *v)
{
    const av_int *i = (const av_int *)v;
    return &av_int_new(i->value)->base.base;
}
