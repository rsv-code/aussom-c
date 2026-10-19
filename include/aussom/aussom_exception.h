/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AUSSOM_EXCEPTION_H
#define AUSSOM_AUSSOM_EXCEPTION_H

/* Engine-level exception, mirrors com.aussom.ast.aussomException.
 * Used to signal parser/dispatcher failures, not user-visible runtime
 * exceptions (those go through the AussomException value type once
 * the runtime layer is ported). Allocated via the project's GC; do
 * not free. */
typedef struct aussom_exception {
    char *message;
    char *trace;
} aussom_exception;

/* Allocates an aussom_exception. The strings are duplicated into GC
 * memory. Either argument may be NULL and is treated as an empty
 * string. */
aussom_exception *aussom_exception_new(const char *message, const char *trace);

#endif /* AUSSOM_AUSSOM_EXCEPTION_H */
