/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_BOOL_H
#define AUSSOM_AV_BOOL_H

#include <stdbool.h>

#include "aussom/av_object.h"

/* Mirrors com.aussom.types.AussomBool (cBool). */
typedef struct av_bool {
    av_object base;
    bool value;
} av_bool;

av_bool *av_bool_new(bool value);
bool av_bool_get(const av_bool *b);
void av_bool_set(av_bool *b, bool value);

#endif /* AUSSOM_AV_BOOL_H */
