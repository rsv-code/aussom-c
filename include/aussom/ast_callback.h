/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_CALLBACK_H
#define AUSSOM_AST_CALLBACK_H

#include "aussom/ast_node.h"

/* Mirrors com.aussom.ast.astCallback, and also com.aussom.ast.astClosure.
 *
 * aussom-base models the closure definition site (::name(args) { ... })
 * as astClosure, a subclass of astCallback that deliberately keeps
 * astNodeType.CALLBACK so every consumer checking for a callback node
 * still sees one; the only difference is an evalImpl that captures the
 * defining invocation's locals. This port dispatches on a tag rather
 * than on a vtable, and the two nodes share one struct layout and one
 * destructor, so the subclass becomes an is_closure flag on this node
 * and a branch in ast_callback_eval. Same observable behavior, and no
 * second tag for a subtype that owns no extra storage. */
typedef struct ast_callback {
    ast_node base;
    char *funct_name;
    /* True for a closure definition site. Such a node always captures
     * the current locals; a plain ::name node captures only when the
     * named function is itself a hoisted closure. */
    bool is_closure;
} ast_callback;

ast_callback *ast_callback_new(void);
ast_callback *ast_callback_new_named(const char *funct_name);
/* Definition-site closure node (= astClosure). */
ast_callback *ast_closure_new_named(const char *funct_name);

void ast_callback_set_funct_name(ast_callback *n, const char *funct_name);
const char *ast_callback_get_funct_name(const ast_callback *n);

#endif /* AUSSOM_AST_CALLBACK_H */
