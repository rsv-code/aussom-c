/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_break.h"

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_break *av_break_new(void)
{
    av_break *b = AUSSOM_ALLOC(sizeof(*b));
    b->base.type = C_TYPE_BREAK;
    return b;
}

void av_break_write(const av_type *v, int level, FILE *out)
{
    (void)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cBreak\"\n", out);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_break_write_str(const av_type *v, int level, FILE *out)
{
    (void)v;
    (void)level;
    fputs("break", out);
}

av_type *av_break_clone(const av_type *v)
{
    (void)v;
    return &av_break_new()->base;
}
