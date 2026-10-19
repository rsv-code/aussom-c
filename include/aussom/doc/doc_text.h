/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_DOC_TEXT_H
#define AUSSOM_DOC_TEXT_H

#include "aussom/doc/doc_type.h"

/* Mirrors com.aussom.ast.doc.docText. The Java side uses inheritance
 * (docAnnotation extends docText). The C port embeds the same
 * fields as the base and a separate doc_annotation struct adds
 * the annotation-specific fields. Both are stored in the doc_list
 * array on astAussomDoc with the doc_type tag selecting which fields
 * are populated. */
typedef struct doc_text {
    doc_type type;
    char *text;
    /* Annotation-only fields. Unused when type == DOC_TYPE_TEXT. */
    char *tag_name;
    char *value;
    char *description;
} doc_text;

doc_text *doc_text_new(const char *text);
doc_text *doc_annotation_new(const char *text);

void doc_text_set_text(doc_text *t, const char *text);
void doc_text_set_tag_name(doc_text *t, const char *name);
void doc_text_set_value(doc_text *t, const char *value);
void doc_text_set_description(doc_text *t, const char *description);

/* Annotation helper: returns a GC-managed string of value plus
 * trailing description (separated by a space) when description is
 * non-empty, else a copy of value. */
char *doc_annotation_get_combined_value(const doc_text *t);

#endif /* AUSSOM_DOC_TEXT_H */
