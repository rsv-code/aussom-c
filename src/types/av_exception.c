/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_exception.h"

#include <stdio.h>
#include <string.h>

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

const char *av_ex_type_name(av_ex_type t)
{
    switch (t) {
    case AV_EX_UNDEF:    return "exUndef";
    case AV_EX_INTERNAL: return "exInternal";
    case AV_EX_RUNTIME:  return "exRuntime";
    }
    return "exUndef";
}

av_exception *av_exception_new_type(av_ex_type et)
{
    av_exception *e = AUSSOM_ALLOC(sizeof(*e));
    av_object_init(&e->base, C_TYPE_EXCEPTION);
    e->et = et;
    e->line_number = -1;
    e->id = AUSSOM_STRDUP("");
    e->text = AUSSOM_STRDUP("");
    e->details = AUSSOM_STRDUP("");
    e->stack_trace = AUSSOM_STRDUP("");
    e->local_object = false;
    e->debugger_seen = false;
    e->cancellation = false;
    e->thrown = false;
    return e;
}

av_exception *av_exception_new(const char *text)
{
    av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
    e->text = AUSSOM_STRDUP(text);
    e->details = AUSSOM_STRDUP(text);
    return e;
}

av_exception *av_exception_new_thrown(const char *thrown)
{
    av_exception *e = av_exception_new(thrown);
    e->id = AUSSOM_STRDUP("EXTERN_INVOCATION_TARGET_EXCEPTION");
    e->thrown = true;
    return e;
}

av_exception *av_exception_new_thrown_aussom(const char *message)
{
    static const char pre[] = "com.aussom.ast.aussomException: ";
    size_t n = sizeof(pre) + strlen(message);
    char *t = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(t, n, "%s%s", pre, message);
    return av_exception_new_thrown(t);
}

void av_exception_set(av_exception *e, int line, const char *id,
                      const char *text, const char *details,
                      const char *stack_trace)
{
    e->line_number = line;
    e->id = AUSSOM_STRDUP(id);
    e->text = AUSSOM_STRDUP(text);
    e->details = AUSSOM_STRDUP(details);
    e->stack_trace = AUSSOM_STRDUP(stack_trace);
}

av_ex_type  av_exception_get_et(const av_exception *e) { return e->et; }
int         av_exception_get_line(const av_exception *e) { return e->line_number; }
const char *av_exception_get_id(const av_exception *e) { return e->id; }
const char *av_exception_get_text(const av_exception *e) { return e->text; }
const char *av_exception_get_details(const av_exception *e) { return e->details; }
const char *av_exception_get_stack_trace(const av_exception *e) { return e->stack_trace; }

void av_exception_set_id(av_exception *e, const char *id) { e->id = AUSSOM_STRDUP(id); }
void av_exception_set_line(av_exception *e, int line) { e->line_number = line; }
void av_exception_set_stack_trace(av_exception *e, const char *st) { e->stack_trace = AUSSOM_STRDUP(st); }

bool av_exception_is_local_object(const av_exception *e) { return e->local_object; }
void av_exception_set_local_object(av_exception *e, bool v) { e->local_object = v; }
bool av_exception_is_debugger_seen(const av_exception *e) { return e->debugger_seen; }
void av_exception_set_debugger_seen(av_exception *e, bool v) { e->debugger_seen = v; }
bool av_exception_is_cancellation(const av_exception *e) { return e->cancellation; }
void av_exception_set_cancellation(av_exception *e, bool v) { e->cancellation = v; }

/* str() / toString() route through the same multi-line formatter as
 * AussomException.stackTraceToString. */
static void write_block(const av_exception *e, int level, FILE *out)
{
    av_write_tabs(level, out);
    fprintf(out, "line %d: [cException] %s Exception\n",
            e->line_number, av_ex_type_name(e->et));
    av_write_tabs(level + 1, out);
    fprintf(out, "id: %s\n", e->id);
    av_write_tabs(level + 1, out);
    fprintf(out, "text: %s\n", e->text);
    av_write_tabs(level + 1, out);
    fprintf(out, "details: %s\n", e->details);
    av_write_tabs(level + 1, out);
    fprintf(out, "stackTrace: %s\n", e->stack_trace);
}

void av_exception_write(const av_type *v, int level, FILE *out)
{
    write_block((const av_exception *)v, level, out);
}

void av_exception_write_str(const av_type *v, int level, FILE *out)
{
    write_block((const av_exception *)v, level, out);
}

av_type *av_exception_clone(const av_type *v)
{
    const av_exception *e = (const av_exception *)v;
    av_exception *n = av_exception_new_type(e->et);
    n->line_number = e->line_number;
    n->id = AUSSOM_STRDUP(e->id);
    n->text = AUSSOM_STRDUP(e->text);
    n->details = AUSSOM_STRDUP(e->details);
    n->stack_trace = AUSSOM_STRDUP(e->stack_trace);
    n->local_object = e->local_object;
    n->debugger_seen = e->debugger_seen;
    n->cancellation = e->cancellation;
    return &n->base.base;
}
