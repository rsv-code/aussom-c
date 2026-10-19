/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/access_type.h"

const char *access_type_name(access_type a)
{
    switch (a) {
    case ACCESS_PRIVATE:   return "aPrivate";
    case ACCESS_PUBLIC:    return "aPublic";
    }
    return "aPrivate";
}
