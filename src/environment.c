/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/environment.h"

#include "aussom/aussom_gc.h"

environment *environment_new(engine *eng)
{
    environment *env = AUSSOM_ALLOC(sizeof(*env));
    env->eng = eng;
    env->ci = NULL;
    env->locals = NULL;
    env->st = NULL;
    env->cur_obj = NULL;
    return env;
}

void environment_set(environment *env, av_object *ci, av_strmap *locals,
                     callstack *st)
{
    env->ci = ci;
    env->locals = locals;
    env->st = st;
}

engine *environment_engine(const environment *env) { return env->eng; }
av_object *environment_class_instance(const environment *env) { return env->ci; }
void environment_set_class_instance(environment *env, av_object *ci) { env->ci = ci; }
av_strmap *environment_locals(const environment *env) { return env->locals; }
void environment_set_locals(environment *env, av_strmap *locals) { env->locals = locals; }
callstack *environment_callstack(const environment *env) { return env->st; }
void environment_set_callstack(environment *env, callstack *st) { env->st = st; }
av_type *environment_cur_obj(const environment *env) { return env->cur_obj; }
void environment_set_cur_obj(environment *env, av_type *cur_obj) { env->cur_obj = cur_obj; }

environment *environment_clone(const environment *env, av_type *cur_obj)
{
    environment *ret = environment_new(env->eng);
    environment_set(ret, env->ci, env->locals, env->st);
    ret->cur_obj = cur_obj;
    return ret;
}

char *environment_stack_trace(const environment *env)
{
    if (env->st == NULL) return AUSSOM_STRDUP("");
    return callstack_get_stack_trace(env->st);
}
