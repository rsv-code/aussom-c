/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_vec.h"

#include "aussom/aussom_gc.h"

av_vec *av_vec_new(void)
{
    av_vec *v = AUSSOM_ALLOC(sizeof(*v));
    v->items = NULL;
    v->count = 0;
    v->capacity = 0;
    pthread_mutex_init(&v->lock, NULL);
    return v;
}

/* Caller must hold the lock. */
static void vec_grow(av_vec *v, size_t need)
{
    if (need <= v->capacity) return;
    size_t nc = (v->capacity == 0) ? 8 : v->capacity * 2;
    while (nc < need) nc *= 2;
    v->items = AUSSOM_REALLOC(v->items, nc * sizeof(*v->items));
    v->capacity = nc;
}

void av_vec_add(av_vec *v, av_type *item)
{
    pthread_mutex_lock(&v->lock);
    vec_grow(v, v->count + 1);
    v->items[v->count++] = item;
    pthread_mutex_unlock(&v->lock);
}

size_t av_vec_size(av_vec *v)
{
    pthread_mutex_lock(&v->lock);
    size_t n = v->count;
    pthread_mutex_unlock(&v->lock);
    return n;
}

av_type *av_vec_get(av_vec *v, size_t index)
{
    pthread_mutex_lock(&v->lock);
    av_type *r = (index < v->count) ? v->items[index] : NULL;
    pthread_mutex_unlock(&v->lock);
    return r;
}

bool av_vec_set(av_vec *v, size_t index, av_type *item)
{
    pthread_mutex_lock(&v->lock);
    bool ok = index < v->count;
    if (ok) v->items[index] = item;
    pthread_mutex_unlock(&v->lock);
    return ok;
}

bool av_vec_insert(av_vec *v, size_t index, av_type *item)
{
    pthread_mutex_lock(&v->lock);
    bool ok = index <= v->count;
    if (ok) {
        vec_grow(v, v->count + 1);
        for (size_t i = v->count; i > index; i--) {
            v->items[i] = v->items[i - 1];
        }
        v->items[index] = item;
        v->count++;
    }
    pthread_mutex_unlock(&v->lock);
    return ok;
}

av_type *av_vec_remove_at(av_vec *v, size_t index)
{
    pthread_mutex_lock(&v->lock);
    av_type *removed = NULL;
    if (index < v->count) {
        removed = v->items[index];
        for (size_t i = index; i + 1 < v->count; i++) {
            v->items[i] = v->items[i + 1];
        }
        v->count--;
        v->items[v->count] = NULL;
    }
    pthread_mutex_unlock(&v->lock);
    return removed;
}

void av_vec_clear(av_vec *v)
{
    pthread_mutex_lock(&v->lock);
    v->count = 0;
    pthread_mutex_unlock(&v->lock);
}

void av_vec_lock(av_vec *v)   { pthread_mutex_lock(&v->lock); }
void av_vec_unlock(av_vec *v) { pthread_mutex_unlock(&v->lock); }

size_t av_vec_size_locked(const av_vec *v) { return v->count; }

av_type *av_vec_get_locked(const av_vec *v, size_t index)
{
    return (index < v->count) ? v->items[index] : NULL;
}

av_type **av_vec_snapshot(av_vec *v, size_t *count)
{
    av_vec_lock(v);
    size_t n = v->count;
    av_type **items = (n > 0) ? AUSSOM_ALLOC(n * sizeof(*items)) : NULL;
    for (size_t i = 0; i < n; i++) items[i] = v->items[i];
    av_vec_unlock(v);
    *count = n;
    return items;
}
