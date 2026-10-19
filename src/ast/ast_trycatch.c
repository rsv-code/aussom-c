/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_trycatch.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_trycatch *ast_trycatch_new(void)
{
    ast_trycatch *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_TRYCATCH;
    n->try_inst_list = ast_statement_list_new();
    n->catch_inst_list = ast_statement_list_new();
    return n;
}

void ast_trycatch_set_try_inst_list(ast_trycatch *n, ast_statement_list *list)
{
    n->try_inst_list = list;
}

ast_statement_list *ast_trycatch_get_try_inst_list(const ast_trycatch *n)
{
    return n->try_inst_list;
}

void ast_trycatch_set_catch_inst_list(ast_trycatch *n, ast_statement_list *list)
{
    n->catch_inst_list = list;
}

ast_statement_list *ast_trycatch_get_catch_inst_list(const ast_trycatch *n)
{
    return n->catch_inst_list;
}

void ast_trycatch_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_trycatch *tc = (const ast_trycatch *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (tc->try_inst_list != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"try\":\n", out);
        ast_node_to_string(&tc->try_inst_list->base, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"catchObject\": \"%s\"\n", n->name);
    if (tc->catch_inst_list != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"catch\":\n", out);
        ast_node_to_string(&tc->catch_inst_list->base, level + 1, out);
        fputc('\n', out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_trycatch_eval(ast_node *n, environment *env, bool getref)
{
    ast_trycatch *tc = (ast_trycatch *)n;
    av_type *ret = &av_null_new()->base.base;
    av_type *except = NULL;

    for (size_t i = 0; i < tc->try_inst_list->statements.count; i++) {
        av_type *tmp = ast_node_eval(tc->try_inst_list->statements.items[i], env, getref);
        if (av_is_break(tmp) || av_is_return(tmp)) { ret = tmp; break; }
        if (av_is_ex(tmp)) { except = tmp; break; }
    }

    if (except != NULL) {
        /* A cancellation is a decision made by the host, not a fault in
         * the program, so the catch block does not get to swallow it.
         * Hand it straight back and let it unwind. */
        if (av_exception_is_cancellation((av_exception *)except)) {
            return except;
        }

        /* Mark the exception as a local (caught) object so it stops
         * propagating and can be inspected in the catch body. */
        av_exception_set_local_object((av_exception *)except, true);
        av_strmap_put(environment_locals(env), n->name, except);
        for (size_t i = 0; i < tc->catch_inst_list->statements.count; i++) {
            av_type *tmp = ast_node_eval(tc->catch_inst_list->statements.items[i], env, getref);
            if (av_is_break(tmp) || av_is_return(tmp) || av_is_ex(tmp)) { ret = tmp; break; }
        }
    }
    return ret;
}
