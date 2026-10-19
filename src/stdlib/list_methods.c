/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The `list` type (com.aussom.types.AussomList). Working slice of the
 * stdlib list methods; the rest lands in Phase 3b.
 */

#include <stdint.h>
#include <string.h>

#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/ast_eval.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

static av_list *self_list(av_type *self) { return (av_list *)self; }

/* Membership equality for the list methods. Primitives and callbacks
 * match by value; objects, lists and maps match by identity. Only
 * containsObjRef ignores this and always compares references. */
static bool values_equal(av_type *a, av_type *b)
{
    return av_value_equals(a, b);
}

static av_type *list_size(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_int_new((int64_t)av_list_size(self_list(self)))->base.base;
}

static av_type *list_isEmpty(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_bool_new(av_list_size(self_list(self)) == 0)->base.base;
}

static av_type *list_add(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list_add(self_list(self), av_list_get(args, 0));
    return self;
}

static av_type *list_get(av_type *self, environment *env, av_list *args)
{
    int64_t i;
    if (!av_list_get_int(args, 0, &i)) return &av_exception_new("list_get: argument 1 must be a int.")->base.base;
    av_type *err = stdlib_check_int_range("list.get", i);
    if (err != NULL) return err;
    av_list *l = self_list(self);
    if (i < 0 || (size_t)i >= av_list_size(l)) {
        av_exception *e = av_exception_new("list.get(): Index out of bounds.");
        av_exception_set_stack_trace(e, environment_stack_trace(env));
        return &e->base.base;
    }
    return av_list_get(l, (size_t)i);
}

static av_type *list_set(av_type *self, environment *env, av_list *args)
{
    int64_t i;
    if (!av_list_get_int(args, 0, &i)) return &av_exception_new("list_set: argument 1 must be a int.")->base.base;
    av_type *err = stdlib_check_int_range("list.set", i);
    if (err != NULL) return err;
    av_list *l = self_list(self);
    if (i < 0 || (size_t)i >= av_list_size(l)) {
        av_exception *e = av_exception_new("list.set(): Index out of bounds.");
        av_exception_set_stack_trace(e, environment_stack_trace(env));
        return &e->base.base;
    }
    av_type *old = av_list_get(l, (size_t)i);
    av_list_set(l, (size_t)i, av_list_get(args, 1));
    return old;
}

static av_type *list_removeAt(av_type *self, environment *env, av_list *args)
{
    int64_t i;
    if (!av_list_get_int(args, 0, &i)) return &av_exception_new("list_removeAt: argument 1 must be a int.")->base.base;
    av_type *err = stdlib_check_int_range("list.removeAt", i);
    if (err != NULL) return err;
    av_list *l = self_list(self);
    av_type *r = av_list_remove_at(l, (size_t)i);
    if (r == NULL) {
        av_exception *e = av_exception_new("list.removeAt(): Index out of bounds.");
        av_exception_set_stack_trace(e, environment_stack_trace(env));
        return &e->base.base;
    }
    return r;
}

static av_type *list_clear(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_list_clear(self_list(self));
    return self;
}

static av_type *list_contains(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_type *target = av_list_get(args, 0);
    size_t n = av_list_size(l);
    for (size_t i = 0; i < n; i++) {
        if (values_equal(av_list_get(l, i), target)) return &av_bool_new(true)->base.base;
    }
    return &av_bool_new(false)->base.base;
}

static av_type *list_indexOf(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_type *target = av_list_get(args, 0);
    size_t n = av_list_size(l);
    for (size_t i = 0; i < n; i++) {
        if (values_equal(av_list_get(l, i), target)) return &av_int_new((int64_t)i)->base.base;
    }
    return &av_int_new(-1)->base.base;
}

static av_type *list_join(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    const char *glue = av_list_get_string(args, 0);
    if (glue == NULL) return &av_exception_new("list_join: argument 1 must be a string.")->base.base;
    size_t glen = strlen(glue);
    size_t cap = 32, len = 0;
    char *out = AUSSOM_ALLOC_ATOMIC(cap);
    out[0] = '\0';
    size_t n = av_list_size(l);
    for (size_t i = 0; i < n; i++) {
        const char *piece = av_str(av_list_get(l, i));
        size_t pl = strlen(piece);
        size_t add = pl + (i > 0 ? glen : 0);
        if (len + add + 1 > cap) {
            while (len + add + 1 > cap) cap *= 2;
            char *nb = AUSSOM_ALLOC_ATOMIC(cap);
            memcpy(nb, out, len);
            out = nb;
        }
        if (i > 0) { memcpy(out + len, glue, glen); len += glen; }
        memcpy(out + len, piece, pl); len += pl;
    }
    out[len] = '\0';
    return &av_string_new(out)->base.base;
}

static av_type *list_addAll(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_list *other = av_list_get_list(args, 0);
    if (other == NULL) return &av_exception_new("list_addAll: argument 1 must be a list.")->base.base;
    size_t n = av_list_size(other);
    for (size_t i = 0; i < n; i++) av_list_add(l, av_list_get(other, i));
    return self;
}

static av_type *list_addAllAt(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_list *other = av_list_get_list(args, 0);
    if (other == NULL) return &av_exception_new("list_addAllAt: argument 1 must be a list.")->base.base;
    int64_t idx;
    if (!av_list_get_int(args, 1, &idx)) return &av_exception_new("list_addAllAt: argument 2 must be a int.")->base.base;
    av_type *err = stdlib_check_int_range("list.addAllAt", idx);
    if (err != NULL) return err;
    /* ArrayList.addAll(index, ...) checks the index first and throws. */
    if (idx < 0 || idx > (int64_t)av_list_size(l)) {
        char m[96];
        snprintf(m, sizeof(m), "java.lang.IndexOutOfBoundsException: Index: %lld, Size: %zu",
                 (long long)idx, av_list_size(l));
        return &av_exception_new_thrown(m)->base.base;
    }
    size_t n = av_list_size(other);
    for (size_t i = 0; i < n; i++) {
        av_vec_insert(av_list_vec(l), (size_t)idx + i, av_list_get(other, i));
    }
    return self;
}

/* AussomList.clone: a new list holding the same elements (a shallow
 * copy, as ArrayList.clone is). */
static av_type *list_clone(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_list *l = self_list(self);
    av_list *n = av_list_new();
    size_t cnt = av_list_size(l);
    for (size_t i = 0; i < cnt; i++) av_list_add(n, av_list_get(l, i));
    return &n->base.base;
}

static av_type *list_containsObjRef(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_type *t = av_list_get(args, 0);
    size_t n = av_list_size(l);
    for (size_t i = 0; i < n; i++) {
        if (av_list_get(l, i) == t) return &av_bool_new(true)->base.base;
    }
    return &av_bool_new(false)->base.base;
}

/* remove(v): drops the first element equal to v (by value for
 * primitives and callbacks, by reference for objects/lists/maps),
 * matching ArrayList.remove(Object) on values with identity equals.
 * Returns the list. */
static av_type *list_remove(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_type *t = av_list_get(args, 0);
    av_vec *vec = av_list_vec(l);
    av_vec_lock(vec);
    for (size_t i = 0; i < vec->count; i++) {
        if (values_equal(vec->items[i], t)) {
            for (size_t j = i; j + 1 < vec->count; j++) vec->items[j] = vec->items[j + 1];
            vec->count--;
            break;
        }
    }
    av_vec_unlock(vec);
    return self;
}

/* A copy of a list's items, taken under its lock and released.
 * removeAll and retainAll read `other` while holding self's lock. When
 * other is self (l.removeAll(l)), reading it under that lock waited on
 * itself forever, so they read this copy instead, as sorting does. */
typedef struct { av_type **items; size_t count; } list_snap;

static list_snap list_snapshot(av_list *l)
{
    list_snap s;
    s.items = av_vec_snapshot(av_list_vec(l), &s.count);
    return s;
}

static bool list_has_ref(const list_snap *s, av_type *t)
{
    for (size_t i = 0; i < s->count; i++) if (values_equal(s->items[i], t)) return true;
    return false;
}

static av_type *list_removeAll(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_list *other = av_list_get_list(args, 0);
    if (other == NULL) return &av_exception_new("list_removeAll: argument 1 must be a list.")->base.base;
    list_snap snap = list_snapshot(other);
    av_vec *vec = av_list_vec(l);
    bool changed = false;
    av_vec_lock(vec);
    size_t w = 0;
    for (size_t i = 0; i < vec->count; i++) {
        if (list_has_ref(&snap, vec->items[i])) { changed = true; }
        else { vec->items[w++] = vec->items[i]; }
    }
    vec->count = w;
    av_vec_unlock(vec);
    return &av_bool_new(changed)->base.base;
}

static av_type *list_retainAll(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *l = self_list(self);
    av_list *other = av_list_get_list(args, 0);
    if (other == NULL) return &av_exception_new("list_retainAll: argument 1 must be a list.")->base.base;
    list_snap snap = list_snapshot(other);
    av_vec *vec = av_list_vec(l);
    bool changed = false;
    av_vec_lock(vec);
    size_t w = 0;
    for (size_t i = 0; i < vec->count; i++) {
        if (list_has_ref(&snap, vec->items[i])) { vec->items[w++] = vec->items[i]; }
        else { changed = true; }
    }
    vec->count = w;
    av_vec_unlock(vec);
    return &av_bool_new(changed)->base.base;
}

static av_type *list_subList(av_type *self, environment *env, av_list *args)
{
    av_list *l = self_list(self);
    int64_t b;
    if (!av_list_get_int(args, 0, &b)) return &av_exception_new("list_subList: argument 1 must be a int.")->base.base;
    av_type *err = stdlib_check_int_range("list.subList", b);
    if (err != NULL) return err;
    int64_t e;
    if (!av_list_get_int(args, 1, &e)) return &av_exception_new("list_subList: argument 2 must be a int.")->base.base;
    err = stdlib_check_int_range("list.subList", e);
    if (err != NULL) return err;
    int64_t sz = (int64_t)av_list_size(l);
    if (b < 0 || e < b || e > sz) {
        av_exception *ex = av_exception_new("list.subList(): Index out of bounds.");
        av_exception_set_stack_trace(ex, environment_stack_trace(env));
        return &ex->base.base;
    }
    av_list *ret = av_list_new();
    for (int64_t i = b; i < e; i++) av_list_add(ret, av_list_get(l, (size_t)i));
    return &ret->base.base;
}

static av_type *list_toJson(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return av_to_json(self);
}

/* --- sorting ----------------------------------------------------- */

typedef int (*elem_cmp)(av_type *a, av_type *b, void *ctx);

/* Snapshot -> insertion sort -> write back. The snapshot means the
 * list lock is not held while a custom comparator runs arbitrary
 * Aussom code (which could touch the same list). */
static av_type *list_sort_with(av_list *l, elem_cmp cmp, void *ctx, av_type **err)
{
    av_vec *vec = av_list_vec(l);
    av_vec_lock(vec);
    size_t n = vec->count;
    av_type **tmp = (n > 0) ? AUSSOM_ALLOC(n * sizeof(*tmp)) : NULL;
    for (size_t i = 0; i < n; i++) tmp[i] = vec->items[i];
    av_vec_unlock(vec);

    for (size_t i = 1; i < n; i++) {
        av_type *key = tmp[i];
        long j = (long)i - 1;
        while (j >= 0 && cmp(tmp[j], key, ctx) > 0) {
            if (*err != NULL) return *err;
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
        if (*err != NULL) return *err;
    }

    av_vec_lock(vec);
    if (vec->count == n) for (size_t i = 0; i < n; i++) vec->items[i] = tmp[i];
    av_vec_unlock(vec);
    return NULL;
}

static int sgn(int c) { return (c > 0) - (c < 0); }

/* String.compareTo order (by UTF-16 unit), as a sign. */
static int cmp_str(const char *x, const char *y)
{
    int64_t c = av_text_compare(x, y);
    return (c > 0) - (c < 0);
}

/* Ports AussomListComparator's non-custom ordering. Same-typed values
 * compare by their natural order; lists, maps and callbacks compare by
 * their str() form, and objects by class name. Mixed types compare
 * numerically when both are numbers, put null first, and otherwise fall
 * back to comparing str() forms. */
static int default_cmp(av_type *a, av_type *b, void *ctx)
{
    (void)ctx;
    if (a->type == b->type) {
        switch (a->type) {
        case C_TYPE_STRING:
            return cmp_str(av_string_get((av_string *)a), av_string_get((av_string *)b));
        case C_TYPE_BOOL: {
            int x = av_bool_get((av_bool *)a) ? 1 : 0;
            int y = av_bool_get((av_bool *)b) ? 1 : 0;
            return sgn(x - y);
        }
        case C_TYPE_INT: {
            int64_t x = av_int_get((av_int *)a), y = av_int_get((av_int *)b);
            return (x > y) - (x < y);
        }
        case C_TYPE_DOUBLE: {
            double x = av_double_get((av_double *)a), y = av_double_get((av_double *)b);
            return (x > y) - (x < y);
        }
        case C_TYPE_LIST:
        case C_TYPE_MAP:
        case C_TYPE_CALLBACK:
            return cmp_str(av_str(a), av_str(b));
        case C_TYPE_OBJECT: {
            ast_class *ca = (ast_class *)av_object_get_class_def((av_object *)a);
            ast_class *cb = (ast_class *)av_object_get_class_def((av_object *)b);
            const char *na = (ca != NULL) ? ca->base.name : "";
            const char *nb = (cb != NULL) ? cb->base.name : "";
            return cmp_str(na, nb);
        }
        case C_TYPE_NULL:
            return 0;
        default:
            return 0;
        }
    }

    bool an = (a->type == C_TYPE_INT || a->type == C_TYPE_DOUBLE);
    bool bn = (b->type == C_TYPE_INT || b->type == C_TYPE_DOUBLE);
    if (an && bn) {
        double x = av_numeric_double(a), y = av_numeric_double(b);
        return (x > y) - (x < y);
    }
    if (a->type == C_TYPE_NULL) return -1;
    if (b->type == C_TYPE_NULL) return 1;
    return cmp_str(av_str(a), av_str(b));
}

static int default_cmp_desc(av_type *a, av_type *b, void *ctx)
{
    return -default_cmp(a, b, ctx);
}

/* AussomListComparator defaults to DESCENDING, so the bare sort()
 * orders high-to-low and sortAsc() orders low-to-high. (Through 1.2.x
 * the comparator applied the sign the wrong way round, which made the
 * two names mean the opposite of what they say; since fixed in
 * aussom-base.) */
static av_type *list_sort(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_type *err = NULL;
    av_type *r = list_sort_with(self_list(self), default_cmp_desc, NULL, &err);
    return (r != NULL) ? r : self;
}

static av_type *list_sortAsc(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_type *err = NULL;
    av_type *r = list_sort_with(self_list(self), default_cmp, NULL, &err);
    return (r != NULL) ? r : self;
}

/* Reverses the list in place and returns it. */
static av_type *list_reverse(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_vec *vec = av_list_vec(self_list(self));
    av_vec_lock(vec);
    size_t n = vec->count;
    for (size_t i = 0; i < n / 2; i++) {
        av_type *t = vec->items[i];
        vec->items[i] = vec->items[n - 1 - i];
        vec->items[n - 1 - i] = t;
    }
    av_vec_unlock(vec);
    return self;
}

struct custom_ctx { environment *env; av_callback *cb; av_type *err; };

static int custom_cmp(av_type *a, av_type *b, void *vctx)
{
    struct custom_ctx *c = vctx;
    if (c->err != NULL) return 0;
    av_list *cargs = av_list_new();
    av_list_add(cargs, a);
    av_list_add(cargs, b);
    av_type *r = callback_call(c->cb, c->env, cargs);
    if (r->type == C_TYPE_INT) {
        int64_t v = av_int_get((av_int *)r);
        return (v > 0) - (v < 0);
    }
    /* AussomListComparator.customCompare: anything but an int, an
     * exception included, stops the sort, and sortCustom returns the
     * message. */
    char m[160];
    snprintf(m, sizeof(m), "AussomListComparator.compareCustom(): Result from "
             "callback expects type of int, but found %s instead.", c_type_name(r->type));
    c->err = &av_exception_new(m)->base.base;
    return 0;
}

static av_type *list_sortCustom(av_type *self, environment *env, av_list *args)
{
    av_callback *cb = av_list_get_callback(args, 0);
    if (cb == NULL) return &av_exception_new("list.sortCustom(): argument 1 must be a callback.")->base.base;
    struct custom_ctx ctx = { env, cb, NULL };
    av_type *err = NULL;
    av_type *r = list_sort_with(self_list(self), custom_cmp, &ctx, &err);
    if (ctx.err != NULL) return ctx.err;
    return (r != NULL) ? r : self;
}

const aussom_extern_method stdlib_aussom_list_methods[] = {
    { "size", list_size },
    { "isEmpty", list_isEmpty },
    { "add", list_add },
    { "addAll", list_addAll },
    { "addAllAt", list_addAllAt },
    { "get", list_get },
    { "set", list_set },
    { "remove", list_remove },
    { "removeAt", list_removeAt },
    { "removeAll", list_removeAll },
    { "retainAll", list_retainAll },
    { "clear", list_clear },
    { "clone", list_clone },
    { "contains", list_contains },
    { "containsObjRef", list_containsObjRef },
    { "indexOf", list_indexOf },
    { "subList", list_subList },
    { "join", list_join },
    { "sort", list_sort },
    { "sortAsc", list_sortAsc },
    { "sortCustom", list_sortCustom },
    { "reverse", list_reverse },
    { "toJson", list_toJson },
    { "pack", stdlib_pack },
    { NULL, NULL }
};
