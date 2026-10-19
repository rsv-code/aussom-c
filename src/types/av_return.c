/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_return.h"

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_return *av_return_new(av_type *value)
{
    av_return *r = AUSSOM_ALLOC(sizeof(*r));
    r->base.type = C_TYPE_RETURN;
    r->value = value;
    return r;
}

av_type *av_return_get(const av_return *r) { return r->value; }
void av_return_set(av_return *r, av_type *value) { r->value = value; }

void av_return_write(const av_type *v, int level, FILE *out)
{
    const av_return *r = (const av_return *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cReturn\",\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"value\":\n", out);
    if (r->value != NULL) av_to_string(r->value, level + 2, out);
    else { av_write_tabs(level + 2, out); fputs("null", out); }
    fputc('\n', out);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_return_write_str(const av_type *v, int level, FILE *out)
{
    (void)v;
    (void)level;
    fputs("return", out);
}

av_type *av_return_clone(const av_type *v)
{
    const av_return *r = (const av_return *)v;
    return &av_return_new(r->value != NULL ? av_clone(r->value) : NULL)->base;
}
