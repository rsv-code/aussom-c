/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <stdio.h>
#include <string.h>

#include "aussom/ast_eval.h"
#include "aussom/ast_new_inst.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_new_inst *ast_new_inst_new(void)
{
    ast_new_inst *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_NEWINST;
    n->args = ast_funct_def_args_list_new();
    return n;
}

ast_new_inst *ast_new_inst_new_named(const char *name)
{
    ast_new_inst *n = ast_new_inst_new();
    ast_node_set_name(&n->base, name);
    return n;
}

void ast_new_inst_add_arg(ast_new_inst *n, ast_node *arg)
{
    ast_funct_def_args_list_add(n->args, arg);
}

void ast_new_inst_set_args(ast_new_inst *n, ast_funct_def_args_list *args)
{
    n->args = args;
}

ast_funct_def_args_list *ast_new_inst_get_args(const ast_new_inst *n)
{
    return n->args;
}

void ast_new_inst_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_new_inst *ni = (const ast_new_inst *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"constructor\": \"%s\"\n", n->name);
    if (ni->args != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"argumentList\":\n", out);
        ast_node_to_string(&ni->args->base, level + 1, out);
        fputs(",\n", out);
    }
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_new_inst_eval(ast_node *n, environment *env, bool getref)
{
    ast_new_inst *ni = (ast_new_inst *)n;
    engine *eng = environment_engine(env);
    ast_class *cls = engine_resolve_class(eng, n->name);
    if (cls == NULL) {
        char msg[320];
        snprintf(msg, sizeof(msg), "Cannot instantiate object of type '%s', "
                 "class definition not found in engine.", n->name);
        av_exception *e = av_exception_new_type(AV_EX_INTERNAL);
        av_exception_set(e, n->line_num, "INSTANTIATION_FAILURE", msg, msg,
                         environment_stack_trace(env));
        return &e->base.base;
    }

    /* A static class is a singleton built once at startup and reached by
     * class name, so `new` on one is always a mistake. Caught here so
     * the script sees the reason rather than a generic wrapper. */
    if (ast_class_get_static(cls)) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "Cannot instantiate static class '%s' with 'new'. Static classes "
            "are singletons and are accessed by class name.", n->name);
        return eval_make_exception(env, n->line_num, "STATIC_INSTANTIATION", msg);
    }

    av_list *cargs = NULL;
    av_type *err = eval_args(ni->args, env, &cargs);
    if (err != NULL) return err;

    av_type *ret = class_instantiate(env, cls, getref, cargs);
    if (ret->type == C_TYPE_EXCEPTION) {
        /* astNewInst reports a throw from instantiation at the `new`. */
        av_exception *e = (av_exception *)ret;
        const char *id = av_exception_get_id(e);
        const char *text = av_exception_get_text(e);
        if (id != NULL && strcmp(id, "INSTANTIATION_FAILURE") == 0 &&
            text != NULL && strcmp(text, CLASS_INSTANTIATION_THREW) == 0) {
            av_exception_set_line(e, n->line_num);
        }
    }
    if (!av_is_ex(ret) && n->child != NULL) {
        environment *tenv = environment_clone(env, ret);
        ret = ast_node_eval(n->child, tenv, getref);
    }
    return ret;
}
