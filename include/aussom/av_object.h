/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_OBJECT_H
#define AUSSOM_AV_OBJECT_H

#include <stdbool.h>
#include <stdio.h>

#include "aussom/av_strmap.h"
#include "aussom/av_type.h"

/* Forward decl; av_object only needs a pointer to the class def. */
struct ast_class;

/* Mirrors com.aussom.types.AussomObject: the object layer that every
 * value type extends. Carries the class definition (for method
 * dispatch, once the engine/Universe layer is ported), a lazily
 * allocated member map, and an extern-object back-pointer.
 *
 * Deferred from the Java original until the engine layer exists:
 * the Mock framework, JSON/pack serialization, and the
 * Universe class-def auto-linkage. class_def stays NULL until then.
 *
 * `members` is allocated on first write (av_object_members), matching
 * the Java laziness -- primitive values never pay for an empty map. */
typedef struct av_object {
    av_type base;
    struct ast_class *class_def;
    av_strmap *members;       /* lazy: NULL until first member added */
    void *extern_object;
    void *mock;               /* lazy mock_state (test mock/spy); NULL until set */
} av_object;

/* Initializes the object fields on an already-allocated value and
 * sets its type tag. Concrete value constructors call this first. */
void av_object_init(av_object *o, c_type type);

av_object *av_object_new(void);

void av_object_set_class_def(av_object *o, struct ast_class *class_def);
struct ast_class *av_object_get_class_def(const av_object *o);

void av_object_set_extern_object(av_object *o, void *extern_object);
void *av_object_get_extern_object(const av_object *o);

/* Java's extern object can be an Aussom value: the empty string a
 * `class s : string` object carries, or the exception a failed parent
 * constructor left behind (astClass.instantiateInheritedClasses). Java
 * tells such a value apart with instanceof. Here it is kept in a small
 * holder that carries a 64-bit marker, which is what
 * av_object_get_extern_value checks. Reading the marker reads the first
 * 8 bytes of whatever the extern object is, so every native state
 * struct a host attaches must be at least that large.
 *
 * av_object_get_extern_value returns the value, or NULL when the
 * extern object is not one (a native state struct, the object itself,
 * or nothing). */
void av_object_set_extern_value(av_object *o, av_type *value);
av_type *av_object_get_extern_value(const av_object *o);

/* How an object with no members prints when its extern object is a
 * native one with a print form of its own (AussomObject.str asks the
 * extern object; ABuffer and ADate answer). The value types cannot see
 * the standard library, so it installs this at engine creation. The
 * function writes the form and returns true, or returns false. */
typedef bool (*av_object_native_str_fn)(const av_object *o, int level, FILE *out);
void av_object_set_native_str(av_object_native_str_fn fn);

/* Lazily allocates and returns the member map. */
av_strmap *av_object_members(av_object *o);

/* Member helpers that do not allocate the map for read-only misses. */
void     av_object_add_member(av_object *o, const char *key, av_type *value);
bool     av_object_contains_member(const av_object *o, const char *key);
av_type *av_object_get_member(const av_object *o, const char *key);

#endif /* AUSSOM_AV_OBJECT_H */
