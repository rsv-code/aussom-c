/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Extern packages: the C replacement for the Java class loader that
 * astClass.loadExternClass and astFunctDef.callExtern go through.
 *
 * An extern class reference splits at its last dot into a package and a
 * class, the same strings aussom-base hands to loadClass:
 *
 *     extern class bool : com.aussom.types.AussomBool { ... }
 *                         package          class
 *
 * A package is registered on one engine with engine_add_extern_package
 * (engine.h). Nothing is shared between engines, and a C function is
 * reachable from a script only if it is listed in a package registered
 * on that engine.
 */

#ifndef AUSSOM_EXTERN_REGISTRY_H
#define AUSSOM_EXTERN_REGISTRY_H

#include "aussom/extern_fn.h"

/* --- what a package author writes ---------------------------------
 *
 * Plain const data. The same structs describe a package compiled into
 * a program and a package exported by a shared library a host loads. */

typedef struct aussom_extern_method {
    const char *name;                       /* "toString" */
    extern_fn   fn;
} aussom_extern_method;

typedef struct aussom_extern_class {
    const char *name;                       /* "AussomBool" */
    const aussom_extern_method *methods;    /* ends with { NULL, NULL } */
} aussom_extern_class;

typedef struct aussom_extern_package {
    const char *name;                       /* "com.aussom.types" */
    const aussom_extern_class *classes;     /* ends with { NULL, NULL } */
} aussom_extern_package;

/* What a loadable library exports, under the symbol name
 * AUSSOM_PACKAGE_ENTRY. aussom-c never loads libraries itself; this is
 * the contract between a host that does and the libraries it loads. */
typedef const aussom_extern_package *(*aussom_package_entry)(void);
#define AUSSOM_PACKAGE_ENTRY "aussom_package"

/* --- the engine's copy ---------------------------------------------
 *
 * engine_add_extern_package copies a package into these, so the caller's
 * tables need not outlive the call. Function pointers are stored, not
 * copied: a library that supplied them must stay loaded. A copy is never
 * changed after it is made, so reading one takes no lock. */

typedef struct extern_class extern_class;
typedef struct extern_package extern_package;

/* Copies `pkg`, or returns NULL when it has no name. Within a class, the
 * first entry for a method name wins; within a package, the first class
 * of a name wins. */
extern_package *extern_package_copy(const aussom_extern_package *pkg);

const char *extern_package_name(const extern_package *pkg);

/* The class named `name` in `pkg`, or NULL. */
const extern_class *extern_package_find_class(const extern_package *pkg,
                                              const char *name);

/* The implementation of `method` in `cls`, or NULL. */
extern_fn extern_class_find_method(const extern_class *cls, const char *method);

#endif /* AUSSOM_EXTERN_REGISTRY_H */
