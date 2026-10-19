/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_RETURN_H
#define AUSSOM_AV_RETURN_H

#include "aussom/av_type.h"

/* Mirrors com.aussom.types.AussomReturn (cReturn). A control carrier
 * that wraps the returned value as it unwinds out of a function. */
typedef struct av_return {
    av_type base;
    av_type *value;
} av_return;

av_return *av_return_new(av_type *value);
av_type *av_return_get(const av_return *r);
void av_return_set(av_return *r, av_type *value);

#endif /* AUSSOM_AV_RETURN_H */
