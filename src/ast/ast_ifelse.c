/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_ifelse.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_ifelse *ast_ifelse_new(void)
{
    ast_ifelse *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_IFELSE;
    n->if_condition = NULL;
    ast_node_array_init(&n->if_else_conditions);
    n->else_instruction_list = ast_statement_list_new();
    return n;
}

void ast_ifelse_set_if_condition(ast_ifelse *n, ast_condition_block *cond)
{
    n->if_condition = cond;
}

ast_condition_block *ast_ifelse_get_if_condition(const ast_ifelse *n)
{
    return n->if_condition;
}

void ast_ifelse_add_else_if(ast_ifelse *n, ast_condition_block *cond)
{
    ast_node_array_add(&n->if_else_conditions, &cond->base);
}

void ast_ifelse_add_else_instruction(ast_ifelse *n, ast_node *inst)
{
    ast_statement_list_add(n->else_instruction_list, inst);
}

void ast_ifelse_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_ifelse *ie = (const ast_ifelse *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (ie->if_condition != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"ifCondition\":\n", out);
        ast_node_to_string(&ie->if_condition->base, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("\"ifElseConditions\": {\n", out);
    for (size_t i = 0; i < ie->if_else_conditions.count; i++) {
        ast_node_to_string(ie->if_else_conditions.items[i], level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputc('}', out);
    if (ie->else_instruction_list != NULL) {
        fputc('\n', out);
        ast_node_write_tabs(level + 1, out);
        fputs("\"elseInstructionList\":\n", out);
        ast_node_to_string(&ie->else_instruction_list->base, level + 1, out);
        fputc('\n', out);
    } else {
        fputc('\n', out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

av_type *ast_ifelse_eval(ast_node *n, environment *env, bool getref)
{
    ast_ifelse *ie = (ast_ifelse *)n;
    av_type *ret = &av_null_new()->base.base;
    bool cond = false;

    av_type *sig = eval_condition(ie->if_condition->expr, env, getref, &cond);
    if (sig != NULL) return sig;

    if (cond) {
        return ast_condition_block_eval(ie->if_condition, env, getref);
    }

    /* else-if chain */
    for (size_t i = 0; i < ie->if_else_conditions.count; i++) {
        ast_condition_block *acb = (ast_condition_block *)ie->if_else_conditions.items[i];
        bool ec = false;
        sig = eval_condition(acb->expr, env, getref, &ec);
        if (sig != NULL) return sig;
        if (ec) {
            return ast_condition_block_eval(acb, env, getref);
        }
    }

    /* else block */
    ast_statement_list *eb = ie->else_instruction_list;
    for (size_t i = 0; i < eb->statements.count; i++) {
        ret = ast_node_eval(eb->statements.items[i], env, getref);
        if (av_is_break_return_except(ret)) return ret;
    }
    return ret;
}
