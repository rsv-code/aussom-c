/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The `callback` type (com.aussom.types.AussomCallback). Bridges the
 * extern `call(...)` method to the class-runtime callback invoker
 * built in Phase 2.
 */

#include "aussom/av.h"
#include "aussom/ast_eval.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

/* The Aussom-side `call(...)` collects its variadic args into a list
 * and forwards to `_call(list args)`, which is this extern. So our
 * single argument is the list of real callback arguments. */
static av_type *cb_call(av_type *self, environment *env, av_list *args)
{
    av_list *call_args;
    if (av_list_size(args) > 0 && av_list_get(args, 0)->type == C_TYPE_LIST) {
        call_args = (av_list *)av_list_get(args, 0);
    } else {
        call_args = args;
    }
    /* An empty callback (`new callback()`) has no object to call on;
     * AussomCallback.call throws a NullPointerException for it. */
    if (av_callback_get_obj((av_callback *)self) == NULL) {
        return &av_exception_new_thrown("java.lang.NullPointerException: Cannot "
            "invoke \"com.aussom.types.AussomObject.getClassDef(com.aussom.Environment)\" "
            "because \"this.obj\" is null")->base.base;
    }
    return callback_call((av_callback *)self, env, call_args);
}

const aussom_extern_method stdlib_aussom_callback_methods[] = {
    { "_call", cb_call },
    { NULL, NULL }
};
