/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/ast_eval.h"
#include "aussom/ast_funct_def.h"

#include <limits.h>
#include <string.h>

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"

ast_funct_def *ast_funct_def_new(void)
{
    ast_funct_def *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_FUNCTDEF;
    n->arg_list = ast_funct_def_args_list_new();
    n->instruction_list = ast_statement_list_new();
    n->is_extern = false;
    n->is_closure = false;
    return n;
}

ast_funct_def *ast_funct_def_new_named(const char *name)
{
    ast_funct_def *n = ast_funct_def_new();
    ast_node_set_name(&n->base, name);
    return n;
}

void ast_funct_def_set_arg_list(ast_funct_def *n, ast_funct_def_args_list *args)
{
    n->arg_list = args;
}

ast_funct_def_args_list *ast_funct_def_get_arg_list(const ast_funct_def *n)
{
    return n->arg_list;
}

void ast_funct_def_set_instruction_list(ast_funct_def *n, ast_statement_list *body)
{
    n->instruction_list = body;
}

ast_statement_list *ast_funct_def_get_instruction_list(const ast_funct_def *n)
{
    return n->instruction_list;
}

void ast_funct_def_set_extern(ast_funct_def *n, bool is_extern)
{
    n->is_extern = is_extern;
}

bool ast_funct_def_get_extern(const ast_funct_def *n)
{
    return n->is_extern;
}

void ast_funct_def_set_closure(ast_funct_def *n, bool is_closure)
{
    n->is_closure = is_closure;
}

bool ast_funct_def_get_closure(const ast_funct_def *n)
{
    return n->is_closure;
}

c_type ast_funct_def_effective_arg_type(const ast_node *arg)
{
    if (arg->type == AST_TYPE_ETCETERA) return C_TYPE_UNDEF;
    /* A declared type wins, even over a null default: `int x = null` is
     * an int slot (registered for null as well, see computeSlotTags in
     * ast_class.c). Only an untyped null default is a wildcard. */
    if (arg->prim_type != C_TYPE_UNDEF) return arg->prim_type;
    switch (arg->type) {
    case AST_TYPE_BOOL:   return C_TYPE_BOOL;
    case AST_TYPE_INT:    return C_TYPE_INT;
    case AST_TYPE_DOUBLE: return C_TYPE_DOUBLE;
    case AST_TYPE_STRING: return C_TYPE_STRING;
    case AST_TYPE_LIST:   return C_TYPE_LIST;
    case AST_TYPE_MAP:    return C_TYPE_MAP;
    case AST_TYPE_OBJ:    return C_TYPE_OBJECT;
    default:              return C_TYPE_UNDEF;  /* VAR and the rest */
    }
}

char ast_funct_def_mangle_char(c_type t)
{
    switch (t) {
    case C_TYPE_BOOL:     return 'b';
    case C_TYPE_INT:      return 'i';
    case C_TYPE_DOUBLE:   return 'd';
    case C_TYPE_STRING:   return 's';
    case C_TYPE_LIST:     return 'l';
    case C_TYPE_MAP:      return 'm';
    case C_TYPE_OBJECT:   return 'o';
    case C_TYPE_CALLBACK: return 'c';
    case C_TYPE_NULL:     return 'n';
    default:              return '*';
    }
}

char *ast_funct_def_signature(const ast_funct_def *n)
{
    const ast_node_array *as = ast_funct_def_args_list_args(n->arg_list);
    char *out = AUSSOM_ALLOC_ATOMIC(as->count * 4 + 1);
    size_t k = 0;
    for (size_t i = 0; i < as->count; i++) {
        if (i > 0) out[k++] = ',';
        if (as->items[i]->type == AST_TYPE_ETCETERA) {
            memcpy(out + k, "...", 3);
            k += 3;
        } else {
            out[k++] = ast_funct_def_mangle_char(
                ast_funct_def_effective_arg_type(as->items[i]));
        }
    }
    out[k] = '\0';
    return out;
}

bool ast_funct_def_has_wildcard(const ast_funct_def *n)
{
    const ast_node_array *as = ast_funct_def_args_list_args(n->arg_list);
    for (size_t i = 0; i < as->count; i++) {
        if (as->items[i]->type == AST_TYPE_ETCETERA) continue;
        if (ast_funct_def_effective_arg_type(as->items[i]) == C_TYPE_UNDEF) return true;
    }
    return false;
}

bool ast_funct_def_is_variadic(const ast_funct_def *n)
{
    const ast_node_array *as = ast_funct_def_args_list_args(n->arg_list);
    if (as->count == 0) return false;
    return as->items[as->count - 1]->type == AST_TYPE_ETCETERA;
}

int ast_funct_def_min_arity(const ast_funct_def *n)
{
    const ast_node_array *as = ast_funct_def_args_list_args(n->arg_list);
    int min = 0;
    for (size_t i = 0; i < as->count; i++) {
        ast_node *arg = as->items[i];
        if (arg->type == AST_TYPE_ETCETERA) break;
        if (arg->type == AST_TYPE_VAR) min++;
        else break;
    }
    return min;
}

int ast_funct_def_max_arity(const ast_funct_def *n)
{
    if (ast_funct_def_is_variadic(n)) return INT_MAX;
    const ast_node_array *as = ast_funct_def_args_list_args(n->arg_list);
    return (int)as->count;
}

void ast_funct_def_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_funct_def *fd = (const ast_funct_def *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (fd->arg_list != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"argumentList\":\n", out);
        ast_node_to_string(&fd->arg_list->base, level + 1, out);
        fputs(",\n", out);
    }
    if (fd->instruction_list != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"statementList\":\n", out);
        ast_node_to_string(&fd->instruction_list->base, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- argument binding -------------------------------------------- */

/* Binds call args (and defaults / variadic etc) into env's locals.
 * Returns an av_bool on success or an av_exception. Declared-type
 * enforcement is deferred -- overload resolution already selected a
 * compatible overload. */
av_type *funct_def_init_args(environment *env, ast_funct_def *fd, av_list *args)
{
    av_strmap *locals = environment_locals(env);
    const ast_node_array *decl = ast_funct_def_args_list_args(fd->arg_list);
    int argc = (int)av_list_size(args);

    for (size_t i = 0; i < decl->count; i++) {
        ast_node *adef = decl->items[i];
        if ((int)i < argc) {
            if (adef->type == AST_TYPE_ETCETERA) {
                av_list *etc = av_list_new();
                for (int j = (int)i; j < argc; j++) av_list_add(etc, av_list_get(args, (size_t)j));
                av_strmap_put(locals, "etc", &etc->base.base);
                return &av_bool_new(true)->base.base;
            }
            av_strmap_put(locals, adef->name, av_list_get(args, i));
        } else if (adef->type == AST_TYPE_ETCETERA) {
            av_strmap_put(locals, "etc", &av_list_new()->base.base);
            return &av_bool_new(true)->base.base;
        } else if (adef->type != AST_TYPE_VAR) {
            /* Default value: evaluate the literal default node. */
            av_type *dv = ast_node_eval(adef, env, false);
            if (av_is_ex(dv)) return dv;
            av_strmap_put(locals, adef->name, dv);
        } else {
            return eval_make_exception(env, fd->base.line_num, "ARGUMENT_NUMBER",
                "Number of arguments does not match the definition.");
        }
    }
    return &av_bool_new(true)->base.base;
}

/* --- function-body execution ------------------------------------- */

av_type *funct_def_run_body(environment *env, bool getref, ast_funct_def *fd)
{
    av_type *ret = &av_null_new()->base.base;
    ast_statement_list *body = fd->instruction_list;
    for (size_t i = 0; i < body->statements.count; i++) {
        av_type *tmp = ast_node_eval(body->statements.items[i], env, getref);
        if (av_is_return(tmp)) { ret = av_return_get((av_return *)tmp); break; }
        if (av_is_ex(tmp)) { ret = tmp; break; }
    }
    return ret;
}
