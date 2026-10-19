/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_ENVIRONMENT_H
#define AUSSOM_ENVIRONMENT_H

#include "aussom/av_object.h"
#include "aussom/av_strmap.h"
#include "aussom/av_type.h"
#include "aussom/callstack.h"
#include "aussom/engine.h"

/* Mirrors com.aussom.Environment: the per-evaluation execution
 * context. Holds the engine, the current class instance (ci, for
 * scope), the local variables (locals -- a thread-safe av_strmap
 * standing in for Java's Members), the call stack, and the current
 * working object (cur_obj, used while walking an object/index path,
 * not for scoping). GC-allocated. */
typedef struct environment {
    engine *eng;
    av_object *ci;
    av_strmap *locals;
    callstack *st;
    av_type *cur_obj;
} environment;

environment *environment_new(engine *eng);

/* Sets ci/locals/callstack together, matching setEnvironment. */
void environment_set(environment *env, av_object *ci, av_strmap *locals,
                     callstack *st);

engine     *environment_engine(const environment *env);
av_object  *environment_class_instance(const environment *env);
void        environment_set_class_instance(environment *env, av_object *ci);
av_strmap  *environment_locals(const environment *env);
void        environment_set_locals(environment *env, av_strmap *locals);
callstack  *environment_callstack(const environment *env);
void        environment_set_callstack(environment *env, callstack *st);
av_type    *environment_cur_obj(const environment *env);
void        environment_set_cur_obj(environment *env, av_type *cur_obj);

/* Clones the environment, keeping ci/locals/callstack but setting a
 * new cur_obj. Mirrors Environment.clone -- used constantly while
 * descending an object/index path. */
environment *environment_clone(const environment *env, av_type *cur_obj);

/* Convenience: current stack trace as a GC string. */
char *environment_stack_trace(const environment *env);

#endif /* AUSSOM_ENVIRONMENT_H */
