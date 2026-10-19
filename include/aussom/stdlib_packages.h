/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_STDLIB_PACKAGES_H
#define AUSSOM_STDLIB_PACKAGES_H

#include "aussom/extern_registry.h"

/* The two built-in packages behind the extern classes in the embedded
 * standard library. engine_new registers both on every engine before
 * it parses lang.aus. */
extern const aussom_extern_package stdlib_types_package;    /* com.aussom.types */
extern const aussom_extern_package stdlib_stdlib_package;   /* com.aussom.stdlib */

/* Installs the print forms of the standard library's native objects
 * (Date, Buffer) for the value types to use. Called by engine_new. */
void stdlib_install_native_str(void);

#endif /* AUSSOM_STDLIB_PACKAGES_H */
