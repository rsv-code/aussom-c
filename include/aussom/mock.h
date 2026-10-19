/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Test mock / spy support (com.aussom Mock). Mock state hangs off an
 * av_object's `mock` field; class_call consults it before normal
 * dispatch (a simple/when mock replaces the call; a spy records it).
 */

#ifndef AUSSOM_MOCK_H
#define AUSSOM_MOCK_H

#include <stdbool.h>

#include "aussom/av_callback.h"
#include "aussom/av_list.h"
#include "aussom/av_object.h"
#include "aussom/av_type.h"
#include "aussom/environment.h"

/* Setters (object.mock / mockWhen / setSpy / getSpy). */
void av_object_set_function_mock(av_object *o, const char *fn, av_type *ret);
void av_object_set_when_mock(av_object *o, const char *fn, av_callback *cond, av_type *ret);
void av_object_set_spy(av_object *o, const char *fn);
av_list *av_object_get_spy(av_object *o, const char *fn);

/* Query / process (used by class_call). */
bool av_object_has_function_mock(av_object *o, const char *fn);
bool av_object_is_spy_set(av_object *o, const char *fn);

/* Returns the mock return value for a call to `fn` (a simple mock, or a
 * when-mock whose condition matched), or NULL to run the real method.
 * May return an av_exception if a when-condition threw. */
av_type *av_object_process_mock(environment *env, av_object *o, const char *fn, av_list *args);

/* Records one call of `fn` (args + result) when a spy is set. */
void av_object_add_spy_record(av_object *o, const char *fn, av_list *args, av_type *ret);

#endif /* AUSSOM_MOCK_H */
