/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_object.h"

#include <stdint.h>

#include "aussom/av_dispatch.h"
#include "aussom/av_string.h"
#include "aussom/aussom_gc.h"

void av_object_init(av_object *o, c_type type)
{
    o->base.type = type;
    o->base.identity = 0;
    /* No class definition is bound here. A value does not need one to
     * exist, only to dispatch, and dispatch always has an environment
     * to resolve it from (class_def_for). Binding one at construction
     * would mean reaching for a process-wide global, which is what let
     * one engine's class definitions leak into another's.
     *
     * Instantiated objects get their definition from class_instantiate,
     * which takes it from the engine that owns the class, so only
     * primitives are left definition-less. */
    o->class_def = NULL;
    o->members = NULL;
    o->extern_object = NULL;
    o->mock = NULL;
}

av_object *av_object_new(void)
{
    av_object *o = AUSSOM_ALLOC(sizeof(*o));
    av_object_init(o, C_TYPE_OBJECT);
    return o;
}

void av_object_set_class_def(av_object *o, struct ast_class *class_def)
{
    o->class_def = class_def;
}

struct ast_class *av_object_get_class_def(const av_object *o)
{
    return o->class_def;
}

void av_object_set_extern_object(av_object *o, void *extern_object)
{
    o->extern_object = extern_object;
}

void *av_object_get_extern_object(const av_object *o)
{
    return o->extern_object;
}

/* The holder an Aussom value is kept in when it is an object's extern
 * object. The marker is what tells it apart from a native state struct;
 * see av_object_set_extern_value. */
#define AV_EXTERN_VALUE_MAGIC 0x6a5d3c1f9e7b2a41ULL

typedef struct extern_value_box {
    uint64_t magic;
    av_type *value;
} extern_value_box;

void av_object_set_extern_value(av_object *o, av_type *value)
{
    extern_value_box *b = AUSSOM_ALLOC(sizeof(*b));
    b->magic = AV_EXTERN_VALUE_MAGIC;
    b->value = value;
    o->extern_object = b;
}

static av_object_native_str_fn g_native_str = NULL;

void av_object_set_native_str(av_object_native_str_fn fn)
{
    g_native_str = fn;
}

av_type *av_object_get_extern_value(const av_object *o)
{
    const void *eo = o->extern_object;
    if (eo == NULL || eo == (const void *)o) return NULL;
    const extern_value_box *b = (const extern_value_box *)eo;
    return (b->magic == AV_EXTERN_VALUE_MAGIC) ? b->value : NULL;
}

av_strmap *av_object_members(av_object *o)
{
    if (o->members == NULL) o->members = av_strmap_new();
    return o->members;
}

void av_object_add_member(av_object *o, const char *key, av_type *value)
{
    av_strmap_put(av_object_members(o), key, value);
}

bool av_object_contains_member(const av_object *o, const char *key)
{
    if (o->members == NULL) return false;
    return av_strmap_contains(o->members, key);
}

av_type *av_object_get_member(const av_object *o, const char *key)
{
    if (o->members == NULL) return NULL;
    return av_strmap_get(o->members, key);
}

/* --- dispatch bodies (generic cObject instances) ----------------- */

av_type *av_object_clone(const av_type *v)
{
    const av_object *o = (const av_object *)v;
    av_object *n = av_object_new();
    n->class_def = o->class_def;
    if (o->members != NULL) {
        const char **keys;
        av_type **vals;
        size_t count = av_strmap_snapshot(o->members, &keys, &vals);
        for (size_t i = 0; i < count; i++) {
            av_object_add_member(n, keys[i], av_clone(vals[i]));
        }
    }
    return &n->base;
}

void av_object_write_str(const av_type *v, int level, FILE *out)
{
    const av_object *o = (const av_object *)v;
    if (o->members != NULL && av_strmap_size(o->members) > 0) {
        const char **keys;
        av_type **vals;
        size_t count = av_strmap_snapshot(o->members, &keys, &vals);
        fputs("{\n", out);
        for (size_t i = 0; i < count; i++) {
            av_write_tabs(level + 1, out);
            fprintf(out, "'%s': ", keys[i]);
            av_write_str(vals[i], level + 1, out);
            if (i + 1 < count) fputc(',', out);
            fputc('\n', out);
        }
        av_write_tabs(level, out);
        fputc('}', out);
    } else {
        /* AussomObject.str: with no members, an object whose extern
         * object is an Aussom value prints that value. Java also does
         * this when the extern object is the object itself, which
         * recurses until the stack runs out; that is a Java bug, so a
         * plain object prints {} here. */
        av_type *ev = av_object_get_extern_value(o);
        if (ev == NULL) {
            if (g_native_str == NULL || !g_native_str(o, level, out)) fputs("{}", out);
        }
        /* Java calls str(Level) here, and AussomString.str(int) always
         * quotes, at any level. */
        else if (ev->type == C_TYPE_STRING) fprintf(out, "\"%s\"", av_string_get((const av_string *)ev));
        else av_write_str(ev, level, out);
    }
}

void av_object_write(const av_type *v, int level, FILE *out)
{
    const av_object *o = (const av_object *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"type\": \"%s\"", c_type_name(o->base.type));
    if (o->members != NULL && av_strmap_size(o->members) > 0) {
        const char **keys;
        av_type **vals;
        size_t count = av_strmap_snapshot(o->members, &keys, &vals);
        fputs(",\n", out);
        av_write_tabs(level + 1, out);
        fputs("\"members\": {\n", out);
        for (size_t i = 0; i < count; i++) {
            av_write_tabs(level + 2, out);
            fprintf(out, "\"%s\":\n", keys[i]);
            av_to_string(vals[i], level + 2, out);
            fputs(",\n", out);
        }
        av_write_tabs(level + 1, out);
        fputc('}', out);
    }
    fputc('\n', out);
    av_write_tabs(level, out);
    fputc('}', out);
}
