/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * aussom_version(): the version compiled into the library. See
 * aussom/version.h.
 */

#include "aussom/version.h"

const char *aussom_version(void)
{
    return AUSSOM_VERSION_STRING;
}
