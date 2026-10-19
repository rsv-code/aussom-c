/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/memstream.h"
#include "aussom/av_type.h"

#include <stdlib.h>
#include <string.h>

#include "aussom/av_bool.h"
#include "aussom/av_callback.h"
#include "aussom/av_dispatch.h"
#include "aussom/av_double.h"
#include "aussom/av_exception.h"
#include "aussom/av_int.h"
#include "aussom/av_list.h"
#include "aussom/av_map.h"
#include "aussom/av_null.h"
#include "aussom/av_string.h"
#include "aussom/aussom_gc.h"
#include "aussom/stack_guard.h"

/* --- predicates -------------------------------------------------- */

bool av_is_ex(const av_type *v)
{
    return v->type == C_TYPE_EXCEPTION
        && !av_exception_is_local_object((const av_exception *)v);
}

bool av_is_null(const av_type *v)   { return v->type == C_TYPE_NULL; }
bool av_is_return(const av_type *v) { return v->type == C_TYPE_RETURN; }
bool av_is_break(const av_type *v)  { return v->type == C_TYPE_BREAK; }

bool av_is_numeric(const av_type *v)
{
    return v->type == C_TYPE_INT || v->type == C_TYPE_DOUBLE
        || v->type == C_TYPE_BOOL;
}

/* --- numeric coercion -------------------------------------------- */

bool av_numeric_bool(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_BOOL:   return av_bool_get((const av_bool *)v);
    case C_TYPE_INT:    return av_int_get((const av_int *)v) != 0;
    case C_TYPE_DOUBLE: return av_double_get((const av_double *)v) != 0.0;
    default:            return false;
    }
}

uint32_t av_identity(av_type *v)
{
    uint32_t id = __atomic_load_n(&v->identity, __ATOMIC_RELAXED);
    if (id != 0) return id;
    /* A counter run through the splitmix64 finalizer: distinct values
     * spread over 31 bits, like Java's. */
    static uint64_t next;
    uint64_t z = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED) * 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z ^= z >> 31;
    id = (uint32_t)(z >> 33);
    if (id == 0) id = 1;
    /* Two threads may get here at once; the first to store wins. */
    uint32_t unset = 0;
    if (!__atomic_compare_exchange_n(&v->identity, &unset, id, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        id = unset;
    return id;
}

int64_t av_double_to_int(double d)
{
    if (d != d) return 0;                                   /* NaN */
    if (d >= 9223372036854775808.0) return INT64_MAX;       /* 2^63 */
    if (d < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

int64_t av_numeric_int(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_INT:    return av_int_get((const av_int *)v);
    case C_TYPE_BOOL:   return av_bool_get((const av_bool *)v) ? 1 : 0;
    case C_TYPE_DOUBLE: return av_double_to_int(av_double_get((const av_double *)v));
    default:            return 0;
    }
}

double av_numeric_double(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_DOUBLE: return av_double_get((const av_double *)v);
    case C_TYPE_INT:    return (double)av_int_get((const av_int *)v);
    case C_TYPE_BOOL:   return av_bool_get((const av_bool *)v) ? 1.0 : 0.0;
    default:            return 0.0;
    }
}

/* --- comparison -------------------------------------------------- */

av_type *av_compare_equal(const av_type *a, const av_type *b)
{
    if (a->type == C_TYPE_NULL || b->type == C_TYPE_NULL) {
        return &av_bool_new(a->type == C_TYPE_NULL && b->type == C_TYPE_NULL)->base.base;
    }
    switch (a->type) {
    case C_TYPE_BOOL:
    case C_TYPE_INT:
    case C_TYPE_DOUBLE:
        if (av_is_numeric(b)) {
            return &av_bool_new(av_numeric_double(a) == av_numeric_double(b))->base.base;
        }
        break;
    case C_TYPE_STRING:
        if (b->type == C_TYPE_STRING) {
            const char *as = av_string_get((const av_string *)a);
            const char *bs = av_string_get((const av_string *)b);
            return &av_bool_new(strcmp(as, bs) == 0)->base.base;
        }
        break;
    case C_TYPE_OBJECT:
        if (b->type == C_TYPE_OBJECT) return &av_bool_new(a == b)->base.base;
        break;
    case C_TYPE_LIST:
        if (b->type == C_TYPE_LIST) return &av_bool_new(a == b)->base.base;
        break;
    case C_TYPE_MAP:
        if (b->type == C_TYPE_MAP) return &av_bool_new(a == b)->base.base;
        break;
    default:
        break;
    }
    return &av_exception_new("Attempting to compare incompatible types.")->base.base;
}

bool av_value_equals(const av_type *a, const av_type *b)
{
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    switch (a->type) {
    case C_TYPE_NULL:
        return b->type == C_TYPE_NULL;
    case C_TYPE_BOOL:
        return b->type == C_TYPE_BOOL
            && av_bool_get((const av_bool *)a) == av_bool_get((const av_bool *)b);
    case C_TYPE_INT:
        return b->type == C_TYPE_INT
            && av_int_get((const av_int *)a) == av_int_get((const av_int *)b);
    case C_TYPE_DOUBLE:
        return b->type == C_TYPE_DOUBLE
            && av_double_get((const av_double *)a) == av_double_get((const av_double *)b);
    case C_TYPE_STRING:
        return b->type == C_TYPE_STRING
            && strcmp(av_string_get((const av_string *)a),
                      av_string_get((const av_string *)b)) == 0;
    case C_TYPE_CALLBACK: {
        if (b->type != C_TYPE_CALLBACK) return false;
        const av_callback *x = (const av_callback *)a;
        const av_callback *y = (const av_callback *)b;
        return x->obj == y->obj && strcmp(x->funct_name, y->funct_name) == 0;
    }
    default:
        /* Objects, lists and maps have no value equality: identity only,
         * already handled by the a == b check above. */
        return false;
    }
}

av_type *av_eval_expression_bool(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_BOOL:   return &av_bool_new(av_bool_get((const av_bool *)v))->base.base;
    case C_TYPE_INT:    return &av_bool_new(av_int_get((const av_int *)v) != 0)->base.base;
    case C_TYPE_DOUBLE: return &av_bool_new(av_double_get((const av_double *)v) != 0.0)->base.base;
    case C_TYPE_STRING: return &av_bool_new(av_string_get((const av_string *)v)[0] != '\0')->base.base;
    case C_TYPE_LIST:   return &av_bool_new(av_list_size((av_list *)v) != 0)->base.base;
    case C_TYPE_MAP:    return &av_bool_new(av_map_size((av_map *)v) != 0)->base.base;
    case C_TYPE_OBJECT: return &av_bool_new(true)->base.base;
    case C_TYPE_NULL:   return &av_bool_new(false)->base.base;
    default:            return &av_exception_new("Unexpected result from condition.")->base.base;
    }
}

/* --- string / dump dispatch -------------------------------------- */

void av_write_tabs(int level, FILE *out)
{
    for (int i = 0; i < level; i++) fputc('\t', out);
}

/* The three walkers below recurse through containers, so each checks
 * the native stack first (see stack_guard.h). On a low stack they stop
 * and leave the thread tripped; render() and the evaluator turn that
 * into a STACK_OVERFLOW exception. A container that holds itself ends
 * this way, as it does in Java.
 *
 * The two printers also track the containers on the current path. Their
 * output is indented one tab per level, so its size grows with the
 * square of the depth: a list holding itself printed about 17 GB over
 * 74 seconds before an 8 MB stack ran low. A container met again inside
 * itself can never finish printing, so the walk trips at once instead.
 * The path lives in the render() that started the walk. */

typedef struct render_path {
    const av_type **items;
    size_t count;
    size_t cap;
} render_path;

static _Thread_local render_path *cur_path;

/* False, with the thread tripped, when v is already being written. */
static bool path_enter(const av_type *v)
{
    render_path *p = cur_path;
    if (p == NULL) return true;
    for (size_t i = 0; i < p->count; i++) {
        if (p->items[i] == v) { stack_guard_trip(); return false; }
    }
    if (p->count == p->cap) {
        size_t cap = (p->cap == 0) ? 16 : p->cap * 2;
        const av_type **items = realloc(p->items, cap * sizeof(*items));
        if (items == NULL) { stack_guard_trip(); return false; }
        p->items = items;
        p->cap = cap;
    }
    p->items[p->count++] = v;
    return true;
}

static void path_leave(void)
{
    if (cur_path != NULL && cur_path->count > 0) cur_path->count--;
}

void av_write_str(const av_type *v, int level, FILE *out)
{
    if (stack_guard_low()) return;
    if (v == NULL) { fputs("null", out); return; }
    switch (v->type) {
    case C_TYPE_NULL:      av_null_write_str(v, level, out); return;
    case C_TYPE_BOOL:      av_bool_write_str(v, level, out); return;
    case C_TYPE_INT:       av_int_write_str(v, level, out); return;
    case C_TYPE_DOUBLE:    av_double_write_str(v, level, out); return;
    case C_TYPE_STRING:    av_string_write_str(v, level, out); return;
    case C_TYPE_LIST:
        if (!path_enter(v)) return;
        av_list_write_str(v, level, out);
        path_leave();
        return;
    case C_TYPE_MAP:
        if (!path_enter(v)) return;
        av_map_write_str(v, level, out);
        path_leave();
        return;
    case C_TYPE_OBJECT:
        if (!path_enter(v)) return;
        av_object_write_str(v, level, out);
        path_leave();
        return;
    case C_TYPE_CALLBACK:  av_callback_write_str(v, level, out); return;
    case C_TYPE_EXCEPTION: av_exception_write_str(v, level, out); return;
    case C_TYPE_REF:       av_ref_write_str(v, level, out); return;
    case C_TYPE_BREAK:     av_break_write_str(v, level, out); return;
    case C_TYPE_RETURN:    av_return_write_str(v, level, out); return;
    case C_TYPE_UNDEF:     break;
    }
    fputs("undef", out);
}

void av_to_string(const av_type *v, int level, FILE *out)
{
    if (stack_guard_low()) return;
    if (v == NULL) { av_write_tabs(level, out); fputs("null", out); return; }
    switch (v->type) {
    case C_TYPE_NULL:      av_null_write(v, level, out); return;
    case C_TYPE_BOOL:      av_bool_write(v, level, out); return;
    case C_TYPE_INT:       av_int_write(v, level, out); return;
    case C_TYPE_DOUBLE:    av_double_write(v, level, out); return;
    case C_TYPE_STRING:    av_string_write(v, level, out); return;
    case C_TYPE_LIST:
        if (!path_enter(v)) return;
        av_list_write(v, level, out);
        path_leave();
        return;
    case C_TYPE_MAP:
        if (!path_enter(v)) return;
        av_map_write(v, level, out);
        path_leave();
        return;
    case C_TYPE_OBJECT:
        if (!path_enter(v)) return;
        av_object_write(v, level, out);
        path_leave();
        return;
    case C_TYPE_CALLBACK:  av_callback_write(v, level, out); return;
    case C_TYPE_EXCEPTION: av_exception_write(v, level, out); return;
    case C_TYPE_REF:       av_ref_write(v, level, out); return;
    case C_TYPE_BREAK:     av_break_write(v, level, out); return;
    case C_TYPE_RETURN:    av_return_write(v, level, out); return;
    case C_TYPE_UNDEF:     break;
    }
    av_write_tabs(level, out);
    fputs("{ \"type\": \"cUndef\" }", out);
}

static char *render(const av_type *v, int level, bool as_str)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    /* Callers use the result as a string without checking, so a stream
     * that cannot be opened (on Windows it is a temporary file, which
     * can fail for reasons other than memory) gives "", never NULL. */
    if (fp == NULL) return AUSSOM_STRDUP("");
    /* Saved and restored, so a render started inside another keeps its
     * own path. */
    render_path path = { NULL, 0, 0 };
    render_path *outer = cur_path;
    cur_path = &path;
    /* Judge only this walk: a mark set before it is kept, not read. */
    bool earlier = stack_guard_tripped();
    stack_guard_clear();
    if (as_str) av_write_str(v, level, fp);
    else av_to_string(v, level, fp);
    bool cut_short = stack_guard_tripped();
    if (earlier) stack_guard_trip();
    cur_path = outer;
    free(path.items);
    aussom_memstream_close(fp, &buf, &len);
    /* A walk cut short by the stack guard wrote part of the value. Hand
     * back nothing of it; the tripped mark becomes the exception. */
    char *out = AUSSOM_STRDUP(cut_short ? "" : buf);
    aussom_memstream_free(buf);
    return out;
}

char *av_str(const av_type *v) { return render(v, 0, true); }
char *av_str_level(const av_type *v, int level) { return render(v, level, true); }
char *av_to_string_alloc(const av_type *v) { return render(v, 0, false); }

/* --- clone dispatch ---------------------------------------------- */

av_type *av_clone(const av_type *v)
{
    if (stack_guard_low()) return &av_null_new()->base.base;
    if (v == NULL) return NULL;
    switch (v->type) {
    case C_TYPE_NULL:      return av_null_clone(v);
    case C_TYPE_BOOL:      return av_bool_clone(v);
    case C_TYPE_INT:       return av_int_clone(v);
    case C_TYPE_DOUBLE:    return av_double_clone(v);
    case C_TYPE_STRING:    return av_string_clone(v);
    case C_TYPE_LIST:      return av_list_clone(v);
    case C_TYPE_MAP:       return av_map_clone(v);
    case C_TYPE_OBJECT:    return av_object_clone(v);
    case C_TYPE_CALLBACK:  return av_callback_clone(v);
    case C_TYPE_EXCEPTION: return av_exception_clone(v);
    case C_TYPE_REF:       return av_ref_clone(v);
    case C_TYPE_BREAK:     return av_break_clone(v);
    case C_TYPE_RETURN:    return av_return_clone(v);
    case C_TYPE_UNDEF:     break;
    }
    return &av_exception_new("clone(): not implemented for type.")->base.base;
}
