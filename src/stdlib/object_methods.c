/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The `object` base type (com.aussom.types.AussomObject): toJson and
 * the test mock framework (mock / mockWhen / setSpy / getSpy).
 *
 * Registering under "object" covers every value type: each declares
 * the method in lang.aus (so overload resolution finds the fdef), and
 * the extern lookup walks the receiver's class up to "object" to find
 * the one C implementation. class_call consults the mock state set
 * here before normal dispatch.
 */

#include "aussom/av.h"
#include "aussom/extern_registry.h"
#include "aussom/mock.h"
#include "stdlib_internal.h"

static av_type *obj_toJson(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return av_to_json(self);
}

static av_type *obj_mock(av_type *self, environment *env, av_list *args)
{
    (void)env;
    const char *fn = av_list_get_string(args, 0);
    if (fn == NULL) return &av_exception_new("obj_mock: argument 1 must be a string.")->base.base;
    av_object_set_function_mock((av_object *)self, fn, av_list_get(args, 1));
    return self;
}

static av_type *obj_mockWhen(av_type *self, environment *env, av_list *args)
{
    (void)env;
    const char *fn = av_list_get_string(args, 0);
    if (fn == NULL) return &av_exception_new("obj_mockWhen: argument 1 must be a string.")->base.base;
    av_callback *cond = av_list_get_callback(args, 1);
    if (cond == NULL) return &av_exception_new("obj_mockWhen: argument 2 must be a callback.")->base.base;
    av_object_set_when_mock((av_object *)self, fn, cond, av_list_get(args, 2));
    return self;
}

static av_type *obj_setSpy(av_type *self, environment *env, av_list *args)
{
    (void)env;
    const char *fn = av_list_get_string(args, 0);
    if (fn == NULL) return &av_exception_new("object.setSpy(): argument 1 must be a string.")->base.base;
    av_object_set_spy((av_object *)self, fn);
    return self;
}

static av_type *obj_getSpy(av_type *self, environment *env, av_list *args)
{
    (void)env;
    const char *fn = av_list_get_string(args, 0);
    if (fn == NULL) return &av_exception_new("object.getSpy(): argument 1 must be a string.")->base.base;
    /* AussomObject.getSpy builds a new list of new record maps, each
     * with its own copy of the argument list. */
    av_list *ret = av_list_new();
    av_list *recs = av_object_get_spy((av_object *)self, fn);
    size_t n = (recs != NULL) ? av_list_size(recs) : 0;
    for (size_t i = 0; i < n; i++) {
        av_map *r = (av_map *)av_list_get(recs, i);
        av_map *c = av_map_new();
        av_map_put(c, "timestamp", av_map_get(r, "timestamp"));
        av_list *a = av_list_new();
        av_list *ra = (av_list *)av_map_get(r, "arguments");
        for (size_t k = 0; k < av_list_size(ra); k++) av_list_add(a, av_list_get(ra, k));
        av_map_put(c, "arguments", &a->base.base);
        av_map_put(c, "returnValue", av_map_get(r, "returnValue"));
        av_list_add(ret, &c->base.base);
    }
    return &ret->base.base;
}

const aussom_extern_method stdlib_aussom_object_methods[] = {
    { "toJson", obj_toJson },
    { "mock", obj_mock },
    { "mockWhen", obj_mockWhen },
    { "setSpy", obj_setSpy },
    { "getSpy", obj_getSpy },
    { "pack", stdlib_pack },
    { NULL, NULL }
};
