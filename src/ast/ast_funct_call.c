/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <stdio.h>
#include <string.h>

#include "aussom/ast_eval.h"
#include "aussom/ast_funct_call.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_funct_call *ast_funct_call_new(void)
{
    ast_funct_call *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_FUNCTCALL;
    n->args = ast_funct_def_args_list_new();
    return n;
}

ast_funct_call *ast_funct_call_new_named(const char *name)
{
    ast_funct_call *n = ast_funct_call_new();
    ast_node_set_name(&n->base, name);
    return n;
}

void ast_funct_call_add_arg(ast_funct_call *n, ast_node *arg)
{
    ast_funct_def_args_list_add(n->args, arg);
}

void ast_funct_call_set_args(ast_funct_call *n, ast_funct_def_args_list *args)
{
    n->args = args;
}

ast_funct_def_args_list *ast_funct_call_get_args(const ast_funct_call *n)
{
    return n->args;
}

void ast_funct_call_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_funct_call *fc = (const ast_funct_call *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (fc->args != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"argumentList\":\n", out);
        ast_node_to_string(&fc->args->base, level + 1, out);
        fputs(",\n", out);
    }
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_funct_call_eval(ast_node *n, environment *env, bool getref)
{
    ast_funct_call *fc = (ast_funct_call *)n;
    av_type *cur = environment_cur_obj(env);
    if (cur == NULL) {
        size_t mn = strlen(n->name) + 160;
        char *msg = AUSSOM_ALLOC_ATOMIC(mn);
        snprintf(msg, mn, "The function call '%s' has no object. Are you "
                 "forgetting 'this' or the object before before the function call?",
                 n->name);
        av_exception *e = av_exception_new_type(AV_EX_INTERNAL);
        av_exception_set(e, n->line_num, "MISSING_OBJECT", msg, msg,
                         environment_stack_trace(env));
        return &e->base.base;
    }

    av_list *cargs = NULL;
    av_type *err = eval_args(fc->args, env, &cargs);
    if (err != NULL) return err;

    /* The receiver's class def: its own when it is an instantiated
     * object, otherwise this engine's definition for its primitive
     * type. Primitives carry no definition of their own. */
    ast_class *cls = class_def_for(env, cur);
    if (cls == NULL) {
        return eval_make_exception(env, n->line_num, "CLASS_DEF_NOT_FOUND",
            "Receiver has no class definition (stdlib dispatch is Phase 3).");
    }

    /* astFunctCall's "Function called." frame: the call site's file
     * and line, the receiver's type, on top of the caller's stack. The
     * callee's "Defined." frame goes on top of this one. */
    callstack *cst = callstack_new_frame(n->file_name, n->line_num,
                                         eval_type_name(cur), n->name,
                                         "Function called.");
    callstack_set_parent(cst, environment_callstack(env));
    environment *cenv = environment_clone(env, cur);
    environment_set_callstack(cenv, cst);

    av_type *ret = class_call(cenv, getref, cls, n->name, cargs);
    if (!av_is_ex(ret) && n->child != NULL) {
        environment *tenv = environment_clone(env, ret);
        ret = ast_node_eval(n->child, tenv, getref);
    }
    return ret;
}
