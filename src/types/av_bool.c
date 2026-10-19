/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_bool.h"

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_bool *av_bool_new(bool value)
{
    av_bool *b = AUSSOM_ALLOC(sizeof(*b));
    av_object_init(&b->base, C_TYPE_BOOL);
    b->value = value;
    return b;
}

bool av_bool_get(const av_bool *b) { return b->value; }
void av_bool_set(av_bool *b, bool value) { b->value = value; }

void av_bool_write(const av_type *v, int level, FILE *out)
{
    const av_bool *b = (const av_bool *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cBool\",\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %s\n", b->value ? "true" : "false");
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_bool_write_str(const av_type *v, int level, FILE *out)
{
    (void)level;
    const av_bool *b = (const av_bool *)v;
    fputs(b->value ? "true" : "false", out);
}

av_type *av_bool_clone(const av_type *v)
{
    const av_bool *b = (const av_bool *)v;
    return &av_bool_new(b->value)->base.base;
}
