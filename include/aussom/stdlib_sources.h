/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_STDLIB_SOURCES_H
#define AUSSOM_STDLIB_SOURCES_H

#include <stddef.h>

/* The embedded base standard library, compiled into the binary from
 * stdlib/aus/ by the Makefile's xxd rule.
 *
 * These strings live in read-only program memory, so every engine's
 * module registry can point at the same bytes. That is deliberate and
 * it does not weaken engine isolation: what isolation is about is the
 * module MAP -- which modules an engine can include, and no engine
 * being able to change that for another -- and each engine owns its
 * own map. Only the source text is shared, and source text is
 * immutable input to a parse rather than state produced by one. Ports
 * the reasoning behind LangRegistry.BASE_SOURCE, which is static there
 * for the same reason; in C it costs nothing at all, because there is
 * no copy to avoid making. */
size_t      stdlib_source_count(void);
const char *stdlib_source_name(size_t i);
const char *stdlib_source_at(size_t i);

/* Returns the embedded standard-library source for a filename
 * (e.g. "lang.aus", "math.aus"), or NULL if there is no such bundled
 * module. Prefer engine_module_source(), which consults the engine's
 * own registry and therefore sees modules a host has added; this
 * reaches the base library only. */
const char *stdlib_source(const char *name);

#endif /* AUSSOM_STDLIB_SOURCES_H */
