/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_VEC_H
#define AUSSOM_AV_VEC_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "aussom/av_type.h"

/* Thread-safe growable array of av_type*. Backs AussomList, and is
 * the C analogue of Java's `synchronized`-everywhere ArrayList: one
 * lock per vector instance (object-level locking). Every operation
 * takes the lock, so callers get atomic single operations; a caller
 * needing a multi-step atomic sequence should hold the lock across
 * it via av_vec_lock / av_vec_unlock.
 *
 * GC-managed: the struct, the backing buffer, and the elements are
 * all collected. The mutex needs no explicit destroy on glibc (a
 * default mutex owns no kernel resource), so letting the collector
 * reclaim it is safe. */
typedef struct av_vec {
    av_type **items;
    size_t count;
    size_t capacity;
    pthread_mutex_t lock;
} av_vec;

av_vec *av_vec_new(void);

/* Single-operation, internally locked. */
void     av_vec_add(av_vec *v, av_type *item);
size_t   av_vec_size(av_vec *v);
av_type *av_vec_get(av_vec *v, size_t index);            /* NULL if out of range */
bool     av_vec_set(av_vec *v, size_t index, av_type *item); /* false if out of range */
bool     av_vec_insert(av_vec *v, size_t index, av_type *item);
av_type *av_vec_remove_at(av_vec *v, size_t index);      /* returns removed or NULL */
void     av_vec_clear(av_vec *v);

/* Explicit lock control for multi-step atomic sequences. The
 * _locked variants assume the caller already holds the lock. */
void     av_vec_lock(av_vec *v);
void     av_vec_unlock(av_vec *v);
size_t   av_vec_size_locked(const av_vec *v);
av_type *av_vec_get_locked(const av_vec *v, size_t index);

/* Copies the items under the lock and releases it. Code that recurses
 * into the items (printing, JSON, clone) reads the copy, so the lock is
 * never held while the same container is reached again through itself:
 * the mutex is not recursive, and a list that contains itself waited on
 * its own lock forever. Returns NULL with *count 0 for an empty vector. */
av_type **av_vec_snapshot(av_vec *v, size_t *count);

#endif /* AUSSOM_AV_VEC_H */
