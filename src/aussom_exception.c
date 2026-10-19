/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/aussom_exception.h"

#include "aussom/aussom_gc.h"

aussom_exception *aussom_exception_new(const char *message, const char *trace)
{
    aussom_exception *e = AUSSOM_ALLOC(sizeof(*e));
    e->message = AUSSOM_STRDUP(message);
    e->trace = AUSSOM_STRDUP(trace);
    return e;
}
