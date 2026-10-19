/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_MAP_H
#define AUSSOM_AV_MAP_H

#include <stdbool.h>
#include <stddef.h>

#include "aussom/av_object.h"
#include "aussom/av_strmap.h"

/* Mirrors com.aussom.types.AussomMap (cMap). The key/value store is
 * an av_strmap, whose per-instance lock provides the thread-safety
 * that ConcurrentHashMap gave on the Java side. Note this is a
 * distinct map from the object's lazy `members` map it inherits via
 * av_object -- exactly as AussomMap has both its CHM value and
 * AussomObject's Members. */
typedef struct av_map {
    av_object base;
    av_strmap *value;
} av_map;

av_map *av_map_new(void);

/* Backing store, for multi-step atomic sequences via av_strmap_lock. */
av_strmap *av_map_strmap(const av_map *m);

void     av_map_put(av_map *m, const char *key, av_type *value);
av_type *av_map_get(av_map *m, const char *key);        /* NULL if absent */
bool     av_map_contains(av_map *m, const char *key);
av_type *av_map_remove(av_map *m, const char *key);
size_t   av_map_size(av_map *m);
void     av_map_clear(av_map *m);

#endif /* AUSSOM_AV_MAP_H */
