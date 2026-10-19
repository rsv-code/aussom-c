/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_BREAK_H
#define AUSSOM_AV_BREAK_H

#include "aussom/av_type.h"

/* Mirrors com.aussom.types.AussomBreak (cBreak). Loop-break signal. */
typedef struct av_break {
    av_type base;
} av_break;

av_break *av_break_new(void);

#endif /* AUSSOM_AV_BREAK_H */
