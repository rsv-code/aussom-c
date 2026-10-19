/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_condition_block.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_condition_block *ast_condition_block_new(void)
{
    ast_condition_block *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_CONDITION;
    n->expr = NULL;
    n->instruction_list = ast_statement_list_new();
    return n;
}

void ast_condition_block_set_expression(ast_condition_block *n, ast_node *expr)
{
    n->expr = expr;
}

ast_node *ast_condition_block_get_expression(const ast_condition_block *n)
{
    return n->expr;
}

void ast_condition_block_add_instruction(ast_condition_block *n, ast_node *inst)
{
    ast_statement_list_add(n->instruction_list, inst);
}

void ast_condition_block_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_condition_block *c = (const ast_condition_block *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (c->expr != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"expression\":\n", out);
        ast_node_to_string(c->expr, level + 1, out);
        fputs(",\n", out);
    }
    if (c->instruction_list != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"statementList\":\n", out);
        ast_node_to_string(&c->instruction_list->base, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- if / else --------------------------------------------------- */

av_type *ast_condition_block_eval(ast_condition_block *cb, environment *env, bool getref)
{
    av_type *ret = &av_null_new()->base.base;
    ast_statement_list *body = cb->instruction_list;
    for (size_t i = 0; i < body->statements.count; i++) {
        av_type *tmp = ast_node_eval(body->statements.items[i], env, getref);
        if (av_is_break_return_except(tmp)) { ret = tmp; break; }
    }
    return ret;
}
