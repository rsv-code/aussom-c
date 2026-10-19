/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_callback.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_callback *ast_callback_new(void)
{
    return ast_callback_new_named("");
}

ast_callback *ast_callback_new_named(const char *funct_name)
{
    ast_callback *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_CALLBACK;
    n->funct_name = AUSSOM_STRDUP(funct_name);
    n->is_closure = false;
    return n;
}

ast_callback *ast_closure_new_named(const char *funct_name)
{
    ast_callback *n = ast_callback_new_named(funct_name);
    n->is_closure = true;
    return n;
}

void ast_callback_set_funct_name(ast_callback *n, const char *funct_name)
{
    n->funct_name = AUSSOM_STRDUP(funct_name);
}

const char *ast_callback_get_funct_name(const ast_callback *n)
{
    return n->funct_name;
}

void ast_callback_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_callback *c = (const ast_callback *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"functName\":\"%s\"\n", c->funct_name);
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_callback_eval(ast_node *n, environment *env, bool getref)
{
    ast_callback *cb = (ast_callback *)n;
    av_object *ci = environment_class_instance(env);
    av_callback *acb = av_callback_new(env, ci,
                                       ast_callback_get_funct_name(cb));

    /* Capture the defining invocation's locals. A closure definition
     * site always captures. A plain ::name captures only when the named
     * function is a closure hoisted onto the current class, so that
     * `scl = ::scale;` written after the definition behaves the same as
     * the definition expression itself. Capture is BY REFERENCE; the
     * copy happens per invocation in class_call_closure. */
    if (cb->is_closure) {
        av_callback_set_captured_locals(acb, environment_locals(env));
    } else if (ci != NULL && av_object_get_class_def(ci) != NULL
               && ast_class_is_closure_function(
                      (ast_class *)av_object_get_class_def(ci),
                      ast_callback_get_funct_name(cb))) {
        av_callback_set_captured_locals(acb, environment_locals(env));
    }

    return eval_child(&acb->base.base, n, env, getref);
}
