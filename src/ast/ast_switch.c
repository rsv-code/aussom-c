/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <stdio.h>

#include "aussom/ast_eval.h"
#include "aussom/c_type.h"
#include "aussom/ast_switch.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_switch *ast_switch_new(void)
{
    ast_switch *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_SWITCH;
    n->expr = NULL;
    ast_node_array_init(&n->case_conditions);
    n->default_list = ast_statement_list_new();
    return n;
}

void ast_switch_set_expression(ast_switch *n, ast_node *expr)
{
    n->expr = expr;
}

ast_node *ast_switch_get_expression(const ast_switch *n)
{
    return n->expr;
}

void ast_switch_add_case(ast_switch *n, ast_condition_block *cond)
{
    ast_node_array_add(&n->case_conditions, &cond->base);
}

void ast_switch_add_default_instruction(ast_switch *n, ast_node *inst)
{
    ast_statement_list_add(n->default_list, inst);
}

void ast_switch_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_switch *s = (const ast_switch *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (s->expr != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"Expression\":\n", out);
        ast_node_to_string(s->expr, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("\"caseConditions\": {\n", out);
    for (size_t i = 0; i < s->case_conditions.count; i++) {
        ast_node_to_string(s->case_conditions.items[i], level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputc('}', out);
    if (s->default_list != NULL) {
        fputc('\n', out);
        ast_node_write_tabs(level + 1, out);
        fputs("\"defaultList\":\n", out);
        ast_node_to_string(&s->default_list->base, level + 1, out);
        fputc('\n', out);
    } else {
        fputc('\n', out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- switch ------------------------------------------------------ */

av_type *ast_switch_eval(ast_node *n, environment *env, bool getref)
{
    ast_switch *sw = (ast_switch *)n;
    av_type *ret = &av_null_new()->base.base;
    av_type *subject = ast_node_eval(sw->expr, env, getref);
    if (av_is_break_return_except(subject)) return subject;

    bool found = false;
    for (size_t i = 0; i < sw->case_conditions.count && !found; i++) {
        ast_condition_block *acb = (ast_condition_block *)sw->case_conditions.items[i];
        av_type *caseval = ast_node_eval(acb->expr, env, getref);
        if (av_is_break_return_except(caseval)) return caseval;
        if (subject->type == caseval->type) {
            av_type *eq = av_compare_equal(subject, caseval);
            if (av_is_ex(eq)) return eq;
            if (av_bool_get((av_bool *)eq)) {
                ret = ast_condition_block_eval(acb, env, getref);
                found = true;
            }
        } else {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "Switch statement expecting type '%s' but found type '%s'.",
                c_type_name(subject->type), c_type_name(caseval->type));
            return eval_make_exception(env, n->line_num, "SWITCH_TYPE_EXCEPTION", msg);
        }
    }
    if (!found) {
        ast_statement_list *def = sw->default_list;
        for (size_t i = 0; i < def->statements.count; i++) {
            ret = ast_node_eval(def->statements.items[i], env, getref);
            if (av_is_break_return_except(ret)) break;
        }
    }
    return ret;
}
