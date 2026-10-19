/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_LIST_H
#define AUSSOM_AV_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aussom/av_object.h"
#include "aussom/av_vec.h"

struct av_map;
struct av_callback;

/* Mirrors com.aussom.types.AussomList (cList). The element store is
 * an av_vec, whose per-instance lock provides the object-level
 * thread-safety that AussomList's `synchronized` methods gave on the
 * Java side. The richer stdlib methods (sort, join, subList, ...)
 * arrive with the engine/dispatch layer; these are the core data
 * operations the interpreter needs directly. */
typedef struct av_list {
    av_object base;
    av_vec *value;
} av_list;

av_list *av_list_new(void);

/* Backing store, for callers that need multi-step atomic sequences
 * via av_vec_lock. */
av_vec *av_list_vec(const av_list *l);

void     av_list_add(av_list *l, av_type *item);
size_t   av_list_size(av_list *l);
av_type *av_list_get(av_list *l, size_t index);             /* NULL if out of range */
bool     av_list_set(av_list *l, size_t index, av_type *item);
bool     av_list_insert(av_list *l, size_t index, av_type *item);
av_type *av_list_remove_at(av_list *l, size_t index);
void     av_list_clear(av_list *l);

/* Typed element getters, for callers (chiefly extern/stdlib methods)
 * that expect an argument of a specific type. They check the element's
 * tag before any cast, so a wrong-typed value can never be
 * reinterpreted as, say, a char* and dereferenced.
 *
 * Reference-typed getters return NULL when the slot is out of range or
 * holds a different type; the caller turns NULL into an exception.
 * Scalar getters return false in that case (0/false are valid data, so
 * failure cannot be a return value) and write *out only on success.
 *
 * av_list_get_double also accepts an int (int->double widening); every
 * other getter is exact. A null element matches none of them. */
const char         *av_list_get_string  (av_list *l, size_t i);
av_list            *av_list_get_list     (av_list *l, size_t i);
struct av_map      *av_list_get_map      (av_list *l, size_t i);
struct av_callback *av_list_get_callback (av_list *l, size_t i);
bool av_list_get_int   (av_list *l, size_t i, int64_t *out);
bool av_list_get_double(av_list *l, size_t i, double  *out);
bool av_list_get_bool  (av_list *l, size_t i, bool    *out);

#endif /* AUSSOM_AV_LIST_H */
