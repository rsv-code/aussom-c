/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_DOC_TYPE_H
#define AUSSOM_DOC_TYPE_H

/* Mirrors com.aussom.ast.doc.docType. */
typedef enum doc_type {
    DOC_TYPE_TEXT = 0,
    DOC_TYPE_ANNOTATION
} doc_type;

const char *doc_type_name(doc_type t);

#endif /* AUSSOM_DOC_TYPE_H */
