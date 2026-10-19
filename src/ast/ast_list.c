/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_list.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_list *ast_list_new(void)
{
    ast_list *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_LIST;
    ast_node_array_init(&n->items);
    return n;
}

void ast_list_add(ast_list *n, ast_node *item)
{
    ast_node_array_add(&n->items, item);
}

size_t ast_list_size(const ast_list *n)
{
    return n->items.count;
}

void ast_list_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_list *l = (const ast_list *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fputs("\"items\": [\n", out);
    for (size_t i = 0; i < l->items.count; i++) {
        ast_node_to_string(l->items.items[i], level + 2, out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("]\n", out);
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- literals ---------------------------------------------------- */

av_type *ast_list_eval(ast_node *n, environment *env, bool getref)
{
    ast_list *al = (ast_list *)n;
    av_list *cl = av_list_new();
    for (size_t i = 0; i < al->items.count; i++) {
        av_type *tmp = ast_node_eval(al->items.items[i], env, getref);
        if (av_is_ex(tmp)) return tmp;
        av_list_add(cl, tmp);
    }
    return eval_child(&cl->base.base, n, env, getref);
}
