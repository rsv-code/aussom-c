/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/doc/doc_type.h"

const char *doc_type_name(doc_type t)
{
    switch (t) {
    case DOC_TYPE_TEXT:       return "TEXT";
    case DOC_TYPE_ANNOTATION: return "ANNOTATION";
    }
    return "TEXT";
}
