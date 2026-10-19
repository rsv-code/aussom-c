/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_INT_H
#define AUSSOM_AV_INT_H

#include <stdint.h>

#include "aussom/av_object.h"

/* Mirrors com.aussom.types.AussomInt (cInt), a signed 64-bit value. */
typedef struct av_int {
    av_object base;
    int64_t value;
} av_int;

av_int *av_int_new(int64_t value);
int64_t av_int_get(const av_int *i);
void av_int_set(av_int *i, int64_t value);

#endif /* AUSSOM_AV_INT_H */
