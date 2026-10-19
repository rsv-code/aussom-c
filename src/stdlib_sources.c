/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/stdlib_sources.h"

#include <stddef.h>
#include <string.h>

/* Defined by the generated build/gen/<name>_aus.c objects. */
extern const char aussom_lang_aus[];
extern const char aussom_math_aus[];
extern const char aussom_sys_aus[];
extern const char aussom_util_aus[];
extern const char aussom_aunit_aus[];
extern const char aussom_reflect_aus[];
extern const char aussom_inctest_aus[];
extern const char aussom_concurrent_aus[];

static const struct {
    const char *name;
    const char *src;
} g_sources[] = {
    { "lang.aus",    aussom_lang_aus },
    { "math.aus",    aussom_math_aus },
    { "sys.aus",     aussom_sys_aus },
    { "util.aus",    aussom_util_aus },
    { "aunit.aus",   aussom_aunit_aus },
    { "reflect.aus", aussom_reflect_aus },
    { "inctest.aus", aussom_inctest_aus },
    { "concurrent.aus", aussom_concurrent_aus },
};

size_t stdlib_source_count(void)
{
    return sizeof(g_sources) / sizeof(g_sources[0]);
}

const char *stdlib_source_name(size_t i)
{
    return (i < stdlib_source_count()) ? g_sources[i].name : NULL;
}

const char *stdlib_source_at(size_t i)
{
    return (i < stdlib_source_count()) ? g_sources[i].src : NULL;
}

const char *stdlib_source(const char *name)
{
    for (size_t i = 0; i < sizeof(g_sources) / sizeof(g_sources[0]); i++) {
        if (strcmp(g_sources[i].name, name) == 0) return g_sources[i].src;
    }
    return NULL;
}
