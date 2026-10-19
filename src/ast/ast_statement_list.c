/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_statement_list.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_statement_list *ast_statement_list_new(void)
{
    ast_statement_list *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_STATEMENTLIST;
    ast_node_array_init(&n->statements);
    return n;
}

ast_statement_list *ast_statement_list_new_named(const char *name)
{
    ast_statement_list *n = ast_statement_list_new();
    ast_node_set_name(&n->base, name);
    return n;
}

void ast_statement_list_add(ast_statement_list *n, ast_node *stmt)
{
    ast_node_array_add(&n->statements, stmt);
}

void ast_statement_list_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_statement_list *s = (const ast_statement_list *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fputs("\"statements\": [\n", out);
    for (size_t i = 0; i < s->statements.count; i++) {
        ast_node_to_string(s->statements.items[i], level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("]\n", out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- statement list ---------------------------------------------- */

av_type *ast_statement_list_eval(ast_node *n, environment *env, bool getref)
{
    ast_statement_list *sl = (ast_statement_list *)n;
    av_type *ret = &av_null_new()->base.base;
    for (size_t i = 0; i < sl->statements.count; i++) {
        ret = ast_node_eval(sl->statements.items[i], env, getref);
        if (av_is_ex(ret) || av_is_return(ret) || av_is_break(ret)) break;
    }
    return ret;
}
