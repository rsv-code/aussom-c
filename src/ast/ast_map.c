/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_map.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_map *ast_map_new(void)
{
    ast_map *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_MAP;
    ast_node_pair_array_init(&n->pairs);
    return n;
}

void ast_map_add(ast_map *n, ast_node *key, ast_node *value)
{
    ast_node_pair_array_add(&n->pairs, key, value);
}

size_t ast_map_size(const ast_map *n)
{
    return n->pairs.count;
}

void ast_map_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_map *m = (const ast_map *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fputs("\"pairs\": [\n", out);
    for (size_t i = 0; i < m->pairs.count; i++) {
        ast_node_write_tabs(level + 2, out);
        fputs("{\n", out);
        ast_node_write_tabs(level + 3, out);
        fputs("\"key\":\n", out);
        ast_node_to_string(m->pairs.items[i].key, level + 3, out);
        fputs(",\n", out);
        ast_node_write_tabs(level + 3, out);
        fputs("\"value\":\n", out);
        ast_node_to_string(m->pairs.items[i].value, level + 3, out);
        ast_node_write_tabs(level + 2, out);
        fputs("},\n", out);
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

/* Map literal key rule (astMap.evalImpl): a VAR key, or an OBJ key
 * with no child, contributes its name literally; anything else is
 * evaluated and stringified. */
av_type *ast_map_eval(ast_node *n, environment *env, bool getref)
{
    ast_map *am = (ast_map *)n;
    av_map *cm = av_map_new();
    for (size_t i = 0; i < am->pairs.count; i++) {
        ast_node *kn = am->pairs.items[i].key;
        ast_node *vn = am->pairs.items[i].value;
        char *kstr;
        if (kn->type == AST_TYPE_VAR
            || (kn->type == AST_TYPE_OBJ && kn->child == NULL)) {
            kstr = AUSSOM_STRDUP(kn->name);
        } else {
            av_type *kval = ast_node_eval(kn, env, getref);
            if (av_is_ex(kval)) return kval;
            kstr = eval_str(kval);
            if (kstr == NULL) return eval_stack_overflow(env, n->line_num);
        }
        av_type *vval = ast_node_eval(vn, env, getref);
        if (av_is_ex(vval)) return vval;
        av_map_put(cm, kstr, vval);
    }
    return eval_child(&cm->base.base, n, env, getref);
}
