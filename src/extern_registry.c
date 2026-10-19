/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/extern_registry.h"

#include <string.h>

#include "aussom/aussom_gc.h"

/* One method in a class's chained hash table. */
typedef struct method_entry {
    const char *name;
    extern_fn fn;
    struct method_entry *next;
} method_entry;

/* Each class is its own allocation, so an ast_class bound to it holds a
 * pointer to the start of an object rather than into the package. */
struct extern_class {
    const char *name;
    method_entry **buckets;
    size_t bucket_count;        /* a power of two */
};

struct extern_package {
    const char *name;
    extern_class **classes;
    size_t class_count;
};

static size_t hash_name(const char *s)
{
    size_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p; h *= 1099511628211ULL;
    }
    return h;
}

static extern_class *class_copy(const aussom_extern_class *src)
{
    size_t n = 0;
    if (src->methods != NULL) {
        while (src->methods[n].name != NULL) n++;
    }
    size_t buckets = 8;
    while (buckets < n * 2) buckets *= 2;

    extern_class *c = AUSSOM_ALLOC(sizeof(*c));
    c->name = AUSSOM_STRDUP(src->name);
    c->bucket_count = buckets;
    c->buckets = AUSSOM_ALLOC(buckets * sizeof(*c->buckets));
    for (size_t i = 0; i < n; i++) {
        const aussom_extern_method *m = &src->methods[i];
        if (extern_class_find_method(c, m->name) != NULL) continue;
        size_t b = hash_name(m->name) & (buckets - 1);
        method_entry *e = AUSSOM_ALLOC(sizeof(*e));
        e->name = AUSSOM_STRDUP(m->name);
        e->fn = m->fn;
        e->next = c->buckets[b];
        c->buckets[b] = e;
    }
    return c;
}

extern_package *extern_package_copy(const aussom_extern_package *src)
{
    if (src == NULL || src->name == NULL) return NULL;
    size_t n = 0;
    if (src->classes != NULL) {
        while (src->classes[n].name != NULL) n++;
    }

    extern_package *p = AUSSOM_ALLOC(sizeof(*p));
    p->name = AUSSOM_STRDUP(src->name);
    p->classes = AUSSOM_ALLOC((n > 0 ? n : 1) * sizeof(*p->classes));
    p->class_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (extern_package_find_class(p, src->classes[i].name) != NULL) continue;
        p->classes[p->class_count++] = class_copy(&src->classes[i]);
    }
    return p;
}

const char *extern_package_name(const extern_package *pkg)
{
    return pkg->name;
}

const extern_class *extern_package_find_class(const extern_package *pkg,
                                              const char *name)
{
    for (size_t i = 0; i < pkg->class_count; i++) {
        if (strcmp(pkg->classes[i]->name, name) == 0) return pkg->classes[i];
    }
    return NULL;
}

extern_fn extern_class_find_method(const extern_class *cls, const char *method)
{
    size_t b = hash_name(method) & (cls->bucket_count - 1);
    for (const method_entry *e = cls->buckets[b]; e != NULL; e = e->next) {
        if (strcmp(e->name, method) == 0) return e->fn;
    }
    return NULL;
}
