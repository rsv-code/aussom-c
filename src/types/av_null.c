/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_null.h"

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_null *av_null_new(void)
{
    av_null *n = AUSSOM_ALLOC(sizeof(*n));
    av_object_init(&n->base, C_TYPE_NULL);
    return n;
}

void av_null_write(const av_type *v, int level, FILE *out)
{
    (void)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cNull\",\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"value\": null\n", out);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_null_write_str(const av_type *v, int level, FILE *out)
{
    (void)v;
    (void)level;
    fputs("null", out);
}

av_type *av_null_clone(const av_type *v)
{
    (void)v;
    return &av_null_new()->base.base;
}
