/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/memstream.h"
#include "aussom/callstack.h"
#include "aussom/stack_guard.h"

#include <stdlib.h>

#include "aussom/aussom_gc.h"

callstack *callstack_new(void)
{
    callstack *cs = AUSSOM_ALLOC(sizeof(*cs));
    cs->parent = NULL;
    cs->file_name = "";
    cs->line_number = -1;
    cs->class_name = "";
    cs->function_name = "";
    cs->text = "";
    cs->called_function = NULL;
    cs->locals = NULL;
    cs->depth = 0;
    stack_guard_bounds(&cs->stack_limit, &cs->stack_high);
    return cs;
}

/* Borrows the string arguments (see the struct comment): they are
 * stored directly, not copied. */
callstack *callstack_new_frame(const char *file_name, int line_number,
                               const char *class_name,
                               const char *function_name, const char *text)
{
    callstack *cs = AUSSOM_ALLOC(sizeof(*cs));
    cs->parent = NULL;
    cs->file_name = file_name;
    cs->line_number = line_number;
    cs->class_name = class_name;
    cs->function_name = function_name;
    cs->text = text;
    cs->called_function = NULL;
    cs->locals = NULL;
    cs->depth = 0;
    stack_guard_bounds(&cs->stack_limit, &cs->stack_high);
    return cs;
}

void callstack_set_parent(callstack *cs, callstack *parent)
{
    cs->parent = parent;
    /* Inherit rather than increment: a frame is not by itself a
     * function call. See the depth field's comment. */
    cs->depth = (parent != NULL) ? parent->depth : 0;
}

void callstack_enter_call(callstack *cs) { cs->depth++; }

int callstack_get_depth(const callstack *cs) { return cs->depth; }
callstack *callstack_get_parent(const callstack *cs) { return cs->parent; }

void callstack_set_called_function(callstack *cs, struct ast_funct_def *f)
{
    cs->called_function = f;
}

struct ast_funct_def *callstack_get_called_function(const callstack *cs)
{
    return cs->called_function;
}

void callstack_set_locals(callstack *cs, struct av_strmap *locals)
{
    cs->locals = locals;
}

struct av_strmap *callstack_get_locals(const callstack *cs)
{
    return cs->locals;
}

static void write_trace(const callstack *cs, FILE *out)
{
    if (cs->class_name[0] != '\0' && cs->function_name[0] != '\0') {
        fprintf(out, "\t[%s:%d] %s { %s.%s() }\n",
                cs->file_name, cs->line_number, cs->text,
                cs->class_name, cs->function_name);
        if (cs->parent != NULL) write_trace(cs->parent, out);
    }
}

char *callstack_get_stack_trace(const callstack *cs)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) return AUSSOM_STRDUP("");
    write_trace(cs, fp);
    aussom_memstream_close(fp, &buf, &len);
    char *out = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return out;
}
