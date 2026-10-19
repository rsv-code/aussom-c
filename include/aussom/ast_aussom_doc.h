/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_AUSSOM_DOC_H
#define AUSSOM_AST_AUSSOM_DOC_H

#include <stdbool.h>

#include "aussom/ast_node.h"
#include "aussom/doc/doc_text.h"

/* Mirrors com.aussom.ast.astAussomDoc. Stores the raw doc-comment
 * text and a parsed list of doc_text entries; the parser splits the
 * text into plain-text paragraphs and `@tag` annotations. */
typedef struct ast_aussom_doc {
    ast_node base;
    char *aussom_doc_text;
    doc_text **doc_list;
    size_t doc_list_count;
    size_t doc_list_capacity;

    /* Whether strip_formatting and parse_text have run for the current
     * text. See the note on ast_aussom_doc_new_text. */
    bool parsed;
} ast_aussom_doc;

/* Constructs an empty doc node. */
ast_aussom_doc *ast_aussom_doc_new(void);

/* Constructs a doc node from raw doc-comment text.
 *
 * Stripping the leading `*` from each line and splitting the result
 * into text and `@tag` entries is deferred to the first read rather
 * than done here. Doc comments are a large share of a typical Aussom
 * source file and nothing on the parse-then-run path looks at the
 * parsed form, so doing the work while parsing charged every engine
 * for something most never ask for. Measured on the bundled standard
 * library, it was about a quarter of process startup. Callers see no
 * difference beyond the first one paying for it. */
ast_aussom_doc *ast_aussom_doc_new_text(const char *text);

/* Constructs a doc node whose text is dropped rather than kept, for
 * when policy says not to retain doc text (aussomdoc.retain false). A
 * node is attached either way, so a consumer gets the same shape
 * whatever policy says; only the text differs. */
ast_aussom_doc *ast_aussom_doc_new_dropped(void);

/* The stripped doc text. Triggers the deferred parse on first call. */
const char *ast_aussom_doc_get_text(ast_aussom_doc *d);

/* The parsed entries: paragraphs of text and `@tag` annotations.
 * Triggers the deferred parse on first call. */
size_t ast_aussom_doc_entry_count(ast_aussom_doc *d);
doc_text *ast_aussom_doc_entry_at(ast_aussom_doc *d, size_t i);

#endif /* AUSSOM_AST_AUSSOM_DOC_H */
