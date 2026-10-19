/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Test mock / spy support. Ports com.aussom.types.Mock: a per-object
 * list of mock entries (simple, when, spy) consulted by class_call.
 */

#include "aussom/mock.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aussom/ast_eval.h"   /* callback_call */
#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/c_type.h"

enum { MOCK_SIMPLE, MOCK_WHEN, MOCK_SPY };

typedef struct mock_fn {
    char *name;
    int kind;
    av_type *ret;        /* simple / when return value */
    av_callback *cond;   /* when condition */
    av_list *spy;        /* spy: one record per call */
} mock_fn;

typedef struct mock_state {
    mock_fn *fns;
    size_t count;
    size_t cap;
} mock_state;

static mock_state *state_of(av_object *o)
{
    if (o->mock == NULL) {
        mock_state *m = AUSSOM_ALLOC(sizeof(*m));
        m->fns = NULL;
        m->count = 0;
        m->cap = 0;
        o->mock = m;
    }
    return (mock_state *)o->mock;
}

static mock_fn *add_entry(mock_state *m)
{
    if (m->count == m->cap) {
        size_t nc = (m->cap == 0) ? 4 : m->cap * 2;
        m->fns = AUSSOM_REALLOC(m->fns, nc * sizeof(*m->fns));
        m->cap = nc;
    }
    mock_fn *e = &m->fns[m->count++];
    e->name = NULL; e->kind = 0; e->ret = NULL; e->cond = NULL; e->spy = NULL;
    return e;
}

void av_object_set_function_mock(av_object *o, const char *fn, av_type *ret)
{
    mock_fn *e = add_entry(state_of(o));
    e->name = AUSSOM_STRDUP(fn);
    e->kind = MOCK_SIMPLE;
    e->ret = ret;
}

void av_object_set_when_mock(av_object *o, const char *fn, av_callback *cond, av_type *ret)
{
    mock_fn *e = add_entry(state_of(o));
    e->name = AUSSOM_STRDUP(fn);
    e->kind = MOCK_WHEN;
    e->cond = cond;
    e->ret = ret;
}

void av_object_set_spy(av_object *o, const char *fn)
{
    mock_fn *e = add_entry(state_of(o));
    e->name = AUSSOM_STRDUP(fn);
    e->kind = MOCK_SPY;
    e->spy = av_list_new();
}

av_list *av_object_get_spy(av_object *o, const char *fn)
{
    if (o->mock != NULL) {
        mock_state *m = (mock_state *)o->mock;
        for (size_t i = 0; i < m->count; i++) {
            if (m->fns[i].kind == MOCK_SPY && strcmp(m->fns[i].name, fn) == 0) {
                return m->fns[i].spy;
            }
        }
    }
    return av_list_new();
}

bool av_object_has_function_mock(av_object *o, const char *fn)
{
    if (o->mock == NULL) return false;
    mock_state *m = (mock_state *)o->mock;
    for (size_t i = 0; i < m->count; i++) {
        if ((m->fns[i].kind == MOCK_SIMPLE || m->fns[i].kind == MOCK_WHEN)
            && strcmp(m->fns[i].name, fn) == 0) return true;
    }
    return false;
}

bool av_object_is_spy_set(av_object *o, const char *fn)
{
    if (o->mock == NULL) return false;
    mock_state *m = (mock_state *)o->mock;
    for (size_t i = 0; i < m->count; i++) {
        if (m->fns[i].kind == MOCK_SPY && strcmp(m->fns[i].name, fn) == 0) return true;
    }
    return false;
}

av_type *av_object_process_mock(environment *env, av_object *o, const char *fn, av_list *args)
{
    mock_state *m = (mock_state *)o->mock;
    if (m == NULL) return NULL;
    /* A simple mock wins outright. */
    for (size_t i = 0; i < m->count; i++) {
        if (m->fns[i].kind == MOCK_SIMPLE && strcmp(m->fns[i].name, fn) == 0) {
            return m->fns[i].ret;
        }
    }
    /* Otherwise the first when-mock whose condition(obj, args) is true. */
    for (size_t i = 0; i < m->count; i++) {
        if (m->fns[i].kind != MOCK_WHEN || strcmp(m->fns[i].name, fn) != 0) continue;
        av_list *fargs = av_list_new();
        av_list_add(fargs, &o->base);
        av_list_add(fargs, &args->base.base);
        av_type *r = callback_call(m->fns[i].cond, env, fargs);
        if (av_is_ex(r)) return r;
        if (r->type != C_TYPE_BOOL && r->type != C_TYPE_INT && r->type != C_TYPE_DOUBLE) {
            char msg[160];
            snprintf(msg, sizeof(msg), "mockWhen(): Condition expecting return type of "
                     "bool, int or double but found %s instead.", c_type_name(r->type));
            return &av_exception_new(msg)->base.base;
        }
        if (av_numeric_bool(r)) return m->fns[i].ret;
    }
    return NULL;
}

/* MockFunctionSpyRecord: the call time in milliseconds since the
 * epoch, the call's arguments and its return value, kept as the map
 * getSpy hands out ({timestamp, arguments, returnValue}). */
void av_object_add_spy_record(av_object *o, const char *fn, av_list *args, av_type *ret)
{
    if (o->mock == NULL) return;
    mock_state *m = (mock_state *)o->mock;
    for (size_t i = 0; i < m->count; i++) {
        if (m->fns[i].kind == MOCK_SPY && strcmp(m->fns[i].name, fn) == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            av_map *rec = av_map_new();
            av_map_put(rec, "timestamp", &av_int_new(ms)->base.base);
            av_map_put(rec, "arguments", &args->base.base);
            av_map_put(rec, "returnValue", ret != NULL ? ret : &av_null_new()->base.base);
            av_list_add(m->fns[i].spy, &rec->base.base);
            return;
        }
    }
}
