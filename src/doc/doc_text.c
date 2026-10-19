/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/doc/doc_text.h"

#include <string.h>

#include "aussom/aussom_gc.h"

doc_text *doc_text_new(const char *text)
{
    doc_text *t = AUSSOM_ALLOC(sizeof(*t));
    t->type = DOC_TYPE_TEXT;
    t->text = AUSSOM_STRDUP(text);
    t->tag_name = AUSSOM_STRDUP("");
    t->value = AUSSOM_STRDUP("");
    t->description = AUSSOM_STRDUP("");
    return t;
}

doc_text *doc_annotation_new(const char *text)
{
    doc_text *t = doc_text_new(text);
    t->type = DOC_TYPE_ANNOTATION;
    return t;
}

void doc_text_set_text(doc_text *t, const char *text)
{
    t->text = AUSSOM_STRDUP(text);
}

void doc_text_set_tag_name(doc_text *t, const char *name)
{
    t->tag_name = AUSSOM_STRDUP(name);
}

void doc_text_set_value(doc_text *t, const char *value)
{
    t->value = AUSSOM_STRDUP(value);
}

void doc_text_set_description(doc_text *t, const char *description)
{
    t->description = AUSSOM_STRDUP(description);
}

char *doc_annotation_get_combined_value(const doc_text *t)
{
    size_t value_len = strlen(t->value);
    size_t desc_len = strlen(t->description);
    if (desc_len == 0) {
        return AUSSOM_STRDUP(t->value);
    }
    char *out = AUSSOM_ALLOC_ATOMIC(value_len + 1 + desc_len + 1);
    memcpy(out, t->value, value_len);
    out[value_len] = ' ';
    memcpy(out + value_len + 1, t->description, desc_len + 1);
    return out;
}
