/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_JSON_H
#define AUSSOM_AV_JSON_H

#include "aussom/av_type.h"

/* Recursively serializes a value to a JSON string (GC-managed).
 * Returns an av_exception value when a non-serializable type is
 * encountered. Shared by the toJson extern on every type. */
av_type *av_to_json(av_type *v);

#endif /* AUSSOM_AV_JSON_H */
