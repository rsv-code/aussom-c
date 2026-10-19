/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The extern method signature. Only forward declarations are used
 * here, so extern_registry.h, which engine.h and ast_class.h include,
 * stays free of every other dependency.
 */

#ifndef AUSSOM_EXTERN_FN_H
#define AUSSOM_EXTERN_FN_H

struct av_type;
struct environment;
struct av_list;

/* An extern method implementation. `self` is the receiver value, which
 * the dispatcher always supplies and may be NULL for a static call.
 * `env` is the execution context (engine, locals, callstack). `args`
 * holds the evaluated call arguments. Returns the result or an
 * av_exception. */
typedef struct av_type *(*extern_fn)(struct av_type *self,
                                     struct environment *env,
                                     struct av_list *args);

#endif /* AUSSOM_EXTERN_FN_H */
