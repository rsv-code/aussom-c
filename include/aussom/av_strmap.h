/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_STRMAP_H
#define AUSSOM_AV_STRMAP_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "aussom/av_type.h"

/* Thread-safe string -> av_type* hash map. The C analogue of the
 * ConcurrentHashMap that backs AussomMap, Members, and the engine
 * class registries. One lock per map instance (object-level
 * locking), coarser than CHM's striping but correct and simple, as
 * agreed for this runtime's concurrency model (no GIL, shared value
 * objects across embedder threads).
 *
 * Iteration order is insertion order, which makes str()/toString()
 * output deterministic (CHM made no such guarantee, so this is a
 * strict improvement for testing). Keys are copied into GC memory.
 * The struct, buckets, order array, and entries are all GC-managed;
 * the mutex needs no destroy on glibc. */
typedef struct av_strmap_entry {
    char *key;
    av_type *value;
    struct av_strmap_entry *next;   /* bucket chain */
} av_strmap_entry;

typedef struct av_strmap {
    av_strmap_entry **buckets;
    size_t bucket_count;
    size_t count;
    /* Insertion-order view used for deterministic iteration. */
    av_strmap_entry **order;
    size_t order_count;
    size_t order_capacity;
    pthread_mutex_t lock;
    bool synchronized;   /* false: skip the lock (frame-private maps) */
} av_strmap;

av_strmap *av_strmap_new(void);

/* Like av_strmap_new but with locking disabled. Use ONLY for maps that
 * never escape a single thread -- specifically call-frame locals, which
 * are frame-private eval state (like the callstack and environment,
 * which are likewise unlocked). Shareable value maps (object members,
 * user maps) MUST use av_strmap_new so cross-thread access stays safe.
 * An unlocked map also uses a plain hash instead of the keyed one (see
 * av_strmap.c), so its keys must not come from data a script reads. */
av_strmap *av_strmap_new_unlocked(void);

/* Inserts or updates. The key is copied. */
void     av_strmap_put(av_strmap *m, const char *key, av_type *value);
av_type *av_strmap_get(av_strmap *m, const char *key);   /* NULL if absent */
bool     av_strmap_contains(av_strmap *m, const char *key);
av_type *av_strmap_remove(av_strmap *m, const char *key); /* removed value or NULL */
size_t   av_strmap_size(av_strmap *m);
void     av_strmap_clear(av_strmap *m);

/* Explicit lock control for iteration / multi-step sequences. The
 * _locked accessors expose the insertion-order view and assume the
 * caller holds the lock. */
void        av_strmap_lock(av_strmap *m);
void        av_strmap_unlock(av_strmap *m);
size_t      av_strmap_size_locked(const av_strmap *m);
const char *av_strmap_key_at_locked(const av_strmap *m, size_t i);
av_type    *av_strmap_value_at_locked(const av_strmap *m, size_t i);

/* Copies the keys and values, in insertion order, under the lock and
 * releases it. The map counterpart of av_vec_snapshot; see there for
 * why. Returns the count; both arrays are NULL when it is 0. */
size_t av_strmap_snapshot(av_strmap *m, const char ***keys, av_type ***values);

#endif /* AUSSOM_AV_STRMAP_H */
