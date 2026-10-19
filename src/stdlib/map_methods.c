/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The `map` type (com.aussom.types.AussomMap). Working slice of the
 * stdlib map methods; the rest lands in Phase 3b.
 */

#include <stdint.h>

#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

static av_map *self_map(av_type *self) { return (av_map *)self; }
static const char *arg_key(av_list *a) { const char *s = av_list_get_string(a, 0); return (s != NULL) ? s : ""; }

static av_type *map_size(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_int_new((int64_t)av_map_size(self_map(self)))->base.base;
}

static av_type *map_isEmpty(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_bool_new(av_map_size(self_map(self)) == 0)->base.base;
}

static av_type *map_put(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_map_put(self_map(self), arg_key(args), av_list_get(args, 1));
    return self;
}

static av_type *map_get(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_type *v = av_map_get(self_map(self), arg_key(args));
    return (v != NULL) ? v : &av_null_new()->base.base;
}

static av_type *map_getd(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_map *m = self_map(self);
    const char *k = arg_key(args);
    if (av_map_contains(m, k)) return av_map_get(m, k);
    return av_list_get(args, 1);   /* default */
}

static av_type *map_containsKey(av_type *self, environment *env, av_list *args)
{
    (void)env;
    return &av_bool_new(av_map_contains(self_map(self), arg_key(args)))->base.base;
}

static av_type *map_remove(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_type *v = av_map_remove(self_map(self), arg_key(args));
    return (v != NULL) ? v : &av_null_new()->base.base;
}

static av_type *map_clear(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_map_clear(self_map(self));
    return self;
}

static av_type *map_keySet(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_strmap *sm = av_map_strmap(self_map(self));
    av_list *keys = av_list_new();
    av_strmap_lock(sm);
    size_t n = av_strmap_size_locked(sm);
    for (size_t i = 0; i < n; i++) {
        av_string *ks = av_string_new(av_strmap_key_at_locked(sm, i));
        av_list_add(keys, &ks->base.base);
    }
    av_strmap_unlock(sm);
    return &keys->base.base;
}

static av_type *map_values(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_strmap *sm = av_map_strmap(self_map(self));
    av_list *vals = av_list_new();
    av_strmap_lock(sm);
    size_t n = av_strmap_size_locked(sm);
    for (size_t i = 0; i < n; i++) {
        av_list_add(vals, av_strmap_value_at_locked(sm, i));
    }
    av_strmap_unlock(sm);
    return &vals->base.base;
}

static av_type *map_containsVal(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_strmap *sm = av_map_strmap(self_map(self));
    av_type *target = av_list_get(args, 0);
    bool found = false;
    av_strmap_lock(sm);
    size_t n = av_strmap_size_locked(sm);
    for (size_t i = 0; i < n; i++) {
        if (av_value_equals(av_strmap_value_at_locked(sm, i), target)) {
            found = true;
            break;
        }
    }
    av_strmap_unlock(sm);
    return &av_bool_new(found)->base.base;
}

static av_type *map_putAll(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_map *dst = self_map(self);
    av_map *srcmap = av_list_get_map(args, 0);
    if (srcmap == NULL) return &av_exception_new("map.putAll(): argument 1 must be a map.")->base.base;
    av_strmap *src = av_map_strmap(srcmap);
    /* Snapshot keys/values so we don't hold the source lock across
     * puts into the destination. */
    av_strmap_lock(src);
    size_t n = av_strmap_size_locked(src);
    const char **keys = (n > 0) ? AUSSOM_ALLOC(n * sizeof(*keys)) : NULL;
    av_type **vals = (n > 0) ? AUSSOM_ALLOC(n * sizeof(*vals)) : NULL;
    for (size_t i = 0; i < n; i++) {
        keys[i] = av_strmap_key_at_locked(src, i);
        vals[i] = av_strmap_value_at_locked(src, i);
    }
    av_strmap_unlock(src);
    for (size_t i = 0; i < n; i++) av_map_put(dst, keys[i], vals[i]);
    return self;
}

static av_type *map_putIfAbsent(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_map *m = self_map(self);
    const char *k = arg_key(args);
    if (!av_map_contains(m, k)) av_map_put(m, k, av_list_get(args, 1));
    return self;
}

static av_type *map_toJson(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return av_to_json(self);
}

const aussom_extern_method stdlib_aussom_map_methods[] = {
    { "size", map_size },
    { "isEmpty", map_isEmpty },
    { "put", map_put },
    { "get", map_get },
    { "getd", map_getd },
    { "containsKey", map_containsKey },
    { "remove", map_remove },
    { "clear", map_clear },
    { "keySet", map_keySet },
    { "values", map_values },
    { "containsVal", map_containsVal },
    { "putAll", map_putAll },
    { "putIfAbsent", map_putIfAbsent },
    { "toJson", map_toJson },
    { "pack", stdlib_pack },
    { NULL, NULL }
};
