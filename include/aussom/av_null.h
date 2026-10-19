/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_NULL_H
#define AUSSOM_AV_NULL_H

#include "aussom/av_object.h"

/* Mirrors com.aussom.types.AussomNull (cNull). No payload. */
typedef struct av_null {
    av_object base;
} av_null;

av_null *av_null_new(void);

#endif /* AUSSOM_AV_NULL_H */
