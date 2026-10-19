/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_DOUBLE_H
#define AUSSOM_AV_DOUBLE_H

#include <stddef.h>

#include "aussom/av_object.h"

/* Mirrors com.aussom.types.AussomDouble (cDouble). */
typedef struct av_double {
    av_object base;
    double value;
} av_double;

av_double *av_double_new(double value);
double av_double_get(const av_double *d);
void av_double_set(av_double *d, double value);

/* Writes Java's Double.toString of `d` into buf. */
void av_double_java_str(double d, char *buf, size_t n);

#endif /* AUSSOM_AV_DOUBLE_H */
