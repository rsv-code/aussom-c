/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_funct_def_args_list.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_funct_def_args_list *ast_funct_def_args_list_new(void)
{
    ast_funct_def_args_list *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_FUNCTDEFARGSLIST;
    ast_node_array_init(&n->args);
    return n;
}

ast_funct_def_args_list *ast_funct_def_args_list_new_named(const char *name)
{
    ast_funct_def_args_list *n = ast_funct_def_args_list_new();
    ast_node_set_name(&n->base, name);
    return n;
}

void ast_funct_def_args_list_add(ast_funct_def_args_list *n, ast_node *arg)
{
    ast_node_array_add(&n->args, arg);
}

const ast_node_array *ast_funct_def_args_list_args(const ast_funct_def_args_list *n)
{
    return &n->args;
}

void ast_funct_def_args_list_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_funct_def_args_list *a = (const ast_funct_def_args_list *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fputs("\"argumentList\": [\n", out);
    for (size_t i = 0; i < a->args.count; i++) {
        ast_node_to_string(a->args.items[i], level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("]\n", out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- functions / instantiation / callbacks ---------------------- */

/* Evaluates a call/constructor argument list into an av_list. Each
 * arg is evaluated with the current object reset to NULL so it
 * resolves in the caller's scope, not the receiver's. Returns NULL
 * and sets *out on success; returns an exception otherwise. */
av_type *eval_args(ast_funct_def_args_list *args, environment *env, av_list **out)
{
    av_list *cargs = av_list_new();
    const ast_node_array *a = ast_funct_def_args_list_args(args);
    for (size_t i = 0; i < a->count; i++) {
        environment *tenv = environment_clone(env, NULL);
        av_type *tmp = ast_node_eval(a->items[i], tenv, false);
        if (av_is_ex(tmp)) return tmp;
        av_list_add(cargs, tmp);
    }
    *out = cargs;
    return NULL;
}
