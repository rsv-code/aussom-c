/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_CALLBACK_H
#define AUSSOM_AV_CALLBACK_H

#include "aussom/av_object.h"

/* Forward decl; the captured environment is opaque to this header to
 * keep the runtime-types layer from depending on the evaluator. */
struct environment;

/* Mirrors com.aussom.types.AussomCallback (cCallback): a bound method
 * reference -- a function name plus the object it is bound to and the
 * environment captured at `obj::funcName`. Invoked via callback_call
 * in the class runtime. */
typedef struct av_callback {
    av_object base;
    char *funct_name;
    av_object *obj;
    struct environment *env;
    /* Non-NULL only when this callback targets a closure. Holds a
     * reference (not a copy) to the defining function invocation's
     * locals, captured when the closure expression -- or ::name on a
     * closure -- was evaluated. Dispatch copies it into the closure's
     * fresh locals on every invocation. */
    av_strmap *captured_locals;
} av_callback;

av_callback *av_callback_new(struct environment *env, av_object *obj,
                             const char *funct_name);

const char *av_callback_get_funct_name(const av_callback *cb);
av_object  *av_callback_get_obj(const av_callback *cb);
struct environment *av_callback_get_env(const av_callback *cb);

av_strmap *av_callback_get_captured_locals(const av_callback *cb);
void av_callback_set_captured_locals(av_callback *cb, av_strmap *locals);

#endif /* AUSSOM_AV_CALLBACK_H */
