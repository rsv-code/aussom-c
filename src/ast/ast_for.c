/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <stdio.h>

#include "aussom/ast_eval.h"
#include "aussom/ast_for.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_for *ast_for_new(void)
{
    ast_for *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_FOR;
    n->expr_init = NULL;
    n->expr_cond = NULL;
    n->expr_inc = NULL;
    n->each_var = NULL;
    n->each_expr = NULL;
    n->is_for_each = false;
    n->instructions = ast_statement_list_new();
    return n;
}

void ast_for_set_expr_init(ast_for *n, ast_node *expr)
{
    n->expr_init = expr;
}

void ast_for_set_expr_cond(ast_for *n, ast_node *expr)
{
    n->expr_cond = expr;
}

void ast_for_set_expr_inc(ast_for *n, ast_node *expr)
{
    n->expr_inc = expr;
}

void ast_for_set_each_var(ast_for *n, ast_node *var)
{
    n->each_var = var;
}

void ast_for_set_each_expr(ast_for *n, ast_node *expr)
{
    n->each_expr = expr;
}

void ast_for_set_is_for_each(ast_for *n, bool is_for_each)
{
    n->is_for_each = is_for_each;
}

void ast_for_add_instruction(ast_for *n, ast_node *inst)
{
    ast_statement_list_add(n->instructions, inst);
}

void ast_for_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_for *f = (const ast_for *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (f->expr_cond != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"condition\":\n", out);
        ast_node_to_string(f->expr_cond, level + 1, out);
        fputs(",\n", out);
    }
    if (f->instructions != NULL) {
        ast_node_to_string(&f->instructions->base, level + 1, out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- for --------------------------------------------------------- */

static av_type *eval_for_each(ast_for *f, environment *env, bool getref)
{
    av_type *ret = &av_null_new()->base.base;
    /* An exception from the collection expression is not returned:
     * like any value that is not a list or map, it ends in
     * FOR_EXCEPTION, as in astFor.evalForEachImpl. */
    av_type *items = ast_node_eval(f->each_expr, env, getref);
    const char *var_name = f->each_var->name;
    av_strmap *locals = environment_locals(env);

    if (items->type == C_TYPE_LIST) {
        av_list *al = (av_list *)items;
        /* The size is read on every pass, as Java's loop does, so a body
         * that shrinks the list ends the loop. av_list_get returns NULL
         * past the end; a list never holds NULL itself. */
        for (size_t i = 0; ; i++) {
            /* Cancellation check on the back edge. See the Cancellation
             * section in engine.h. */
            av_type *cancelled = eval_check_control(&f->base, env);
            if (cancelled != NULL) return cancelled;

            av_type *item = av_list_get(al, i);
            if (item == NULL) break;
            av_strmap_put(locals, var_name, item);
            for (size_t j = 0; j < f->instructions->statements.count; j++) {
                ret = ast_node_eval(f->instructions->statements.items[j], env, getref);
                if (av_is_break_return_except(ret)) break;
            }
            if (av_is_break_return_except(ret)) {
                if (!av_is_ex(ret) && av_is_break_event(ret)) ret = &av_null_new()->base.base;
                break;
            }
        }
    } else if (items->type == C_TYPE_MAP) {
        av_map *am = (av_map *)items;
        av_strmap *sm = av_map_strmap(am);
        /* Snapshot the keys so the map lock is not held across the
         * body. Java walks a ConcurrentHashMap's key set, which never
         * returns a key removed before the walk reaches it, so a key
         * the body has since removed (or cleared) is skipped. */
        const char **keys;
        av_type **unused;
        size_t sz = av_strmap_snapshot(sm, &keys, &unused);
        for (size_t i = 0; i < sz; i++) {
            /* Cancellation check on the back edge. See the Cancellation
             * section in engine.h. */
            av_type *cancelled = eval_check_control(&f->base, env);
            if (cancelled != NULL) return cancelled;

            if (!av_strmap_contains(sm, keys[i])) continue;
            av_strmap_put(locals, var_name, &av_string_new(keys[i])->base.base);
            for (size_t j = 0; j < f->instructions->statements.count; j++) {
                ret = ast_node_eval(f->instructions->statements.items[j], env, getref);
                if (av_is_break_return_except(ret)) break;
            }
            if (av_is_break_return_except(ret)) break;
        }
        if (av_is_break_return_except(ret) && !av_is_ex(ret) && av_is_break_event(ret)) {
            ret = &av_null_new()->base.base;
        }
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "For loop expecting of type LIST or MAP, "
                 "found '%s' instead.", c_type_name(items->type));
        return eval_make_exception(env, f->base.line_num, "FOR_EXCEPTION", msg);
    }
    return ret;
}

static av_type *eval_for_classic(ast_for *f, environment *env, bool getref)
{
    av_type *ret = &av_null_new()->base.base;
    /* astFor.evalForImpl ignores the results of the init and step
     * expressions, exceptions included. */
    (void)ast_node_eval(f->expr_init, env, getref);

    bool cond = false;
    av_type *sig = eval_condition(f->expr_cond, env, getref, &cond);
    if (sig != NULL) return sig;
    while (cond) {
        /* Cancellation check on the back edge. See the Cancellation
         * section in engine.h. */
        av_type *cancelled = eval_check_control(&f->base, env);
        if (cancelled != NULL) return cancelled;

        for (size_t i = 0; i < f->instructions->statements.count; i++) {
            ret = ast_node_eval(f->instructions->statements.items[i], env, getref);
            if (av_is_break_return_except(ret)) break;
        }
        if (av_is_break_return_except(ret)) {
            if (!av_is_ex(ret) && av_is_break_event(ret)) ret = &av_null_new()->base.base;
            break;
        }
        (void)ast_node_eval(f->expr_inc, env, getref);
        sig = eval_condition(f->expr_cond, env, getref, &cond);
        if (sig != NULL) return sig;
    }
    return ret;
}

av_type *ast_for_eval(ast_node *n, environment *env, bool getref)
{
    ast_for *f = (ast_for *)n;
    if (f->is_for_each) return eval_for_each(f, env, getref);
    return eval_for_classic(f, env, getref);
}
