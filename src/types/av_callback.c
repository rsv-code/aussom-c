/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/av_callback.h"

#include <stdint.h>

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_callback *av_callback_new(struct environment *env, av_object *obj,
                             const char *funct_name)
{
    av_callback *cb = AUSSOM_ALLOC(sizeof(*cb));
    av_object_init(&cb->base, C_TYPE_CALLBACK);
    cb->funct_name = AUSSOM_STRDUP(funct_name);
    cb->obj = obj;
    cb->env = env;
    cb->captured_locals = NULL;
    return cb;
}

const char *av_callback_get_funct_name(const av_callback *cb) { return cb->funct_name; }
av_object *av_callback_get_obj(const av_callback *cb) { return cb->obj; }
struct environment *av_callback_get_env(const av_callback *cb) { return cb->env; }

av_strmap *av_callback_get_captured_locals(const av_callback *cb)
{
    return cb->captured_locals;
}

void av_callback_set_captured_locals(av_callback *cb, av_strmap *locals)
{
    cb->captured_locals = locals;
}

/* Identifies the bound object by class name and reference identity,
 * e.g. "dog@1b2c3d", or "null" when unbound. Naming the object this way
 * (rather than rendering it) is what keeps toString from recursing
 * forever when an object holds a callback bound to itself. */
static void write_obj_ref(const av_callback *cb, FILE *out)
{
    if (cb->obj == NULL) {
        fputs("null", out);
        return;
    }
    /* A name-only site with no environment in scope, so it cannot
     * resolve a primitive's definition and does not need to: the type
     * name is what is wanted. Ports the getTypeName() treatment Java
     * gave its 24 name-only getClassDef() sites. */
    ast_class *cd = (ast_class *)av_object_get_class_def(cb->obj);
    const char *name = (cd != NULL) ? cd->base.name
                                    : c_type_name(cb->obj->base.type);
    fprintf(out, "%s@%x", name, (unsigned)av_identity(&cb->obj->base));
}

void av_callback_write(const av_type *v, int level, FILE *out)
{
    const av_callback *cb = (const av_callback *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cCallback\",\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"obj\": \"", out);
    write_obj_ref(cb, out);
    fputs("\",\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"functName\": \"%s\"\n", cb->funct_name);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_callback_write_str(const av_type *v, int level, FILE *out)
{
    (void)level;
    const av_callback *cb = (const av_callback *)v;
    fputs("callback ", out);
    write_obj_ref(cb, out);
    fprintf(out, "::%s", cb->funct_name);
}

av_type *av_callback_clone(const av_type *v)
{
    const av_callback *cb = (const av_callback *)v;
    av_callback *out = av_callback_new(cb->env, cb->obj, cb->funct_name);
    out->captured_locals = cb->captured_locals;
    return &out->base.base;
}


av_type *callback_call(av_callback *cb, environment *env, av_list *args)
{
    av_object *obj = av_callback_get_obj(cb);
    if (obj == NULL || class_def_for(env, &obj->base) == NULL) {
        return eval_make_exception(env, 0, "CALLBACK_UNBOUND",
            "Callback has no bound object.");
    }
    /* Pin curObj and ci to the bound owner for the call, then
     * restore -- mirrors AussomCallback.callWithException. */
    av_type *tobj = environment_cur_obj(env);
    av_object *tci = environment_class_instance(env);
    environment_set_cur_obj(env, &obj->base);
    environment_set_class_instance(env, obj);

    ast_class *ac = class_def_for(env, &obj->base);
    /* The captured scope has to cross the dispatch boundary explicitly:
     * at fire time env's locals belong to the INVOKER, not to the
     * closure's defining invocation. Null for every non-closure
     * callback, in which case class_call_closure behaves exactly like
     * class_call. */
    av_type *ret = class_call_closure(env, false, ac,
                                      av_callback_get_funct_name(cb), args,
                                      cb->captured_locals);

    environment_set_cur_obj(env, tobj);
    environment_set_class_instance(env, tci);
    return ret;
}
