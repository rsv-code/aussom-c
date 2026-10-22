/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/memstream.h"
#include "aussom/ast_eval.h"
#include "aussom/ast_node.h"

#include <stdlib.h>
#include <string.h>

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"
#include "aussom/stack_guard.h"

/* --- ast_node_array ----------------------------------------------- */

void ast_node_array_init(ast_node_array *a)
{
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

void ast_node_array_add(ast_node_array *a, ast_node *node)
{
    if (a->count == a->capacity) {
        size_t new_cap = (a->capacity == 0) ? 4 : a->capacity * 2;
        a->items = AUSSOM_REALLOC(a->items, new_cap * sizeof(*a->items));
        a->capacity = new_cap;
    }
    a->items[a->count++] = node;
}

/* --- ast_node_pair_array ------------------------------------------ */

void ast_node_pair_array_init(ast_node_pair_array *a)
{
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

void ast_node_pair_array_add(ast_node_pair_array *a, ast_node *key, ast_node *value)
{
    if (a->count == a->capacity) {
        size_t new_cap = (a->capacity == 0) ? 4 : a->capacity * 2;
        a->items = AUSSOM_REALLOC(a->items, new_cap * sizeof(*a->items));
        a->capacity = new_cap;
    }
    a->items[a->count].key = key;
    a->items[a->count].value = value;
    a->count++;
}

/* --- ast_node base ------------------------------------------------ */

void ast_node_init(ast_node *n)
{
    n->name = AUSSOM_STRDUP("");
    n->type = AST_TYPE_UNDEF;
    n->file_name = AUSSOM_STRDUP("");
    n->line_num = 0;
    n->col_num = 0;
    n->line_num_end = 0;
    n->col_num_end = 0;
    n->access = ACCESS_PRIVATE;
    n->prim_type = C_TYPE_UNDEF;
    n->child = NULL;
    n->doc_node = NULL;
    ast_node_array_init(&n->annotations);
    n->breakpoint = false;
}

void ast_node_set_name(ast_node *n, const char *name)
{
    n->name = AUSSOM_STRDUP(name);
}

void ast_node_set_file_name(ast_node *n, const char *file_name)
{
    n->file_name = AUSSOM_STRDUP(file_name);
}

void ast_node_set_type(ast_node *n, ast_node_type t)
{
    n->type = t;
}

void ast_node_append_child(ast_node *n, ast_node *child)
{
    ast_node *cur = n;
    while (cur->child != NULL) cur = cur->child;
    cur->child = child;
}

void ast_node_set_parser_info(ast_node *n, const char *file_name,
                              int line_num, int col_num)
{
    ast_node_set_file_name(n, file_name);
    n->line_num = line_num;
    n->col_num = col_num;
    n->line_num_end = line_num;
    n->col_num_end = col_num;
}

void ast_node_set_parser_info_range(ast_node *n, const char *file_name,
                                    int line_num, int col_num,
                                    int line_num_end, int col_num_end)
{
    ast_node_set_file_name(n, file_name);
    n->line_num = line_num;
    n->col_num = col_num;
    n->line_num_end = line_num_end;
    n->col_num_end = col_num_end;
}

/* --- toString dispatch -------------------------------------------- */

void ast_node_write_tabs(int level, FILE *out)
{
    for (int i = 0; i < level; i++) fputc('\t', out);
}

void ast_node_write_node_str(const ast_node *n, int level, FILE *out)
{
    ast_node_write_tabs(level, out);
    fprintf(out, "\"type\": \"%s\",\n", ast_node_type_name(n->type));
    ast_node_write_tabs(level, out);
    fprintf(out, "\"name\": \"%s\",\n", n->name);
    ast_node_write_tabs(level, out);
    fprintf(out, "\"line\": %d", n->line_num);
    if (n->type != AST_TYPE_UNDEF) {
        fprintf(out, ",\n");
        ast_node_write_tabs(level, out);
        fprintf(out, "\"dataType\": \"%s\"", ast_node_type_name(n->type));
    }
    if (n->prim_type != C_TYPE_UNDEF) {
        fprintf(out, ",\n");
        ast_node_write_tabs(level, out);
        fprintf(out, "\"primativeType\": \"%s\"", c_type_name(n->prim_type));
    }
}

void ast_node_to_string(const ast_node *n, int level, FILE *out)
{
    if (n == NULL) {
        ast_node_write_tabs(level, out);
        fputs("null", out);
        return;
    }
    switch (n->type) {
    case AST_TYPE_NULL:             ast_null_write_body(n, level, out); return;
    case AST_TYPE_BOOL:             ast_bool_write_body(n, level, out); return;
    case AST_TYPE_INT:              ast_int_write_body(n, level, out); return;
    case AST_TYPE_DOUBLE:           ast_double_write_body(n, level, out); return;
    case AST_TYPE_STRING:           ast_string_write_body(n, level, out); return;
    case AST_TYPE_LIST:             ast_list_write_body(n, level, out); return;
    case AST_TYPE_MAP:              ast_map_write_body(n, level, out); return;
    case AST_TYPE_INCLUDE:          ast_include_write_body(n, level, out); return;
    case AST_TYPE_CLASS:            ast_class_write_body(n, level, out); return;
    case AST_TYPE_OBJ:              ast_obj_write_body(n, level, out); return;
    case AST_TYPE_VAR:              ast_var_write_body(n, level, out); return;
    case AST_TYPE_FUNCTDEF:         ast_funct_def_write_body(n, level, out); return;
    case AST_TYPE_FUNCTDEFARGSLIST: ast_funct_def_args_list_write_body(n, level, out); return;
    case AST_TYPE_STATEMENTLIST:    ast_statement_list_write_body(n, level, out); return;
    case AST_TYPE_EXP:              ast_expression_write_body(n, level, out); return;
    case AST_TYPE_FUNCTCALL:        ast_funct_call_write_body(n, level, out); return;
    case AST_TYPE_RETURN:           ast_return_write_body(n, level, out); return;
    case AST_TYPE_TRYCATCH:         ast_trycatch_write_body(n, level, out); return;
    case AST_TYPE_THROW:            ast_throw_write_body(n, level, out); return;
    case AST_TYPE_NEWINST:          ast_new_inst_write_body(n, level, out); return;
    case AST_TYPE_IFELSE:           ast_ifelse_write_body(n, level, out); return;
    case AST_TYPE_CONDITION:        ast_condition_block_write_body(n, level, out); return;
    case AST_TYPE_SWITCH:           ast_switch_write_body(n, level, out); return;
    case AST_TYPE_WHILE:            ast_while_write_body(n, level, out); return;
    case AST_TYPE_FOR:              ast_for_write_body(n, level, out); return;
    case AST_TYPE_BREAK:            ast_break_write_body(n, level, out); return;
    case AST_TYPE_ETCETERA:         ast_etcetera_write_body(n, level, out); return;
    case AST_TYPE_CALLBACK:         ast_callback_write_body(n, level, out); return;
    case AST_TYPE_AUSSOM_DOC:       ast_aussom_doc_write_body(n, level, out); return;
    case AST_TYPE_ANNOTATION:       ast_annotation_write_body(n, level, out); return;
    case AST_TYPE_ANNOTATION_ARG:   ast_annotation_arg_write_body(n, level, out); return;
    case AST_TYPE_UNDEF:            break;
    }
    /* Fallback for AST_TYPE_UNDEF or unknown tags. */
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputc('\n', out);
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

char *ast_node_to_string_alloc(const ast_node *n)
{
    /* The memstream buffer is libc-malloc'd; the libc free
     * call is correct here. The contents are copied into GC memory
     * so the returned pointer obeys the project's "no manual free"
     * rule. */
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) return AUSSOM_STRDUP("");   /* see render in av_type.c */
    ast_node_to_string(n, 0, fp);
    aussom_memstream_close(fp, &buf, &len);
    char *gc_copy = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return gc_copy;
}

/* --- control-signal predicates ----------------------------------- */

bool av_is_break_return_event(const av_type *v)
{
    return v != NULL && (v->type == C_TYPE_RETURN || v->type == C_TYPE_BREAK);
}

bool av_is_break_event(const av_type *v)
{
    return v != NULL && v->type == C_TYPE_BREAK;
}

bool av_is_break_return_except(const av_type *v)
{
    return av_is_break_return_event(v) || (v != NULL && v->type == C_TYPE_EXCEPTION);
}

/* --- shared exception constructor -------------------------------- */

av_type *eval_make_exception(environment *env, int line, const char *id,
                             const char *text)
{
    av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
    char *trace = environment_stack_trace(env);
    av_exception_set(e, line, id, text, text, trace);
    return &e->base.base;
}

/* The cancellation exception, built at `line`. */
static av_type *make_cancelled(environment *env, int line)
{
    av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
    char *trace = environment_stack_trace(env);
    av_exception_set(e, line, ENGINE_CANCELLED_EXCEPTION_ID,
                     "Execution cancelled.",
                     "Execution was cancelled by the host via engine_cancel().",
                     trace);
    av_exception_set_cancellation(e, true);
    return &e->base.base;
}

av_type *eval_check_control(ast_node *n, environment *env)
{
    engine *eng = environment_engine(env);
    if (eng == NULL) return NULL;
    /* Fast path: one read and a compare. */
    if (engine_get_control_state(eng) == CONTROL_RUNNING) return NULL;
    /* Blocks here while paused; true only when the program is to stop. */
    if (!engine_await_resume_or_cancel(eng)) return NULL;
    return make_cancelled(env, n->line_num);
}

av_type *eval_check_control_env(environment *env)
{
    if (env == NULL) return NULL;
    engine *eng = environment_engine(env);
    if (eng == NULL) return NULL;
    if (engine_get_control_state(eng) == CONTROL_RUNNING) return NULL;
    if (!engine_await_resume_or_cancel(eng)) return NULL;
    return make_cancelled(env, 0);
}

av_type *eval_check_call_gate(ast_node *n, environment *env, const char *funct_name)
{
    engine *eng = environment_engine(env);
    if (eng == NULL) return NULL;

    if (engine_get_control_state(eng) != CONTROL_RUNNING) {
        if (engine_await_resume_or_cancel(eng)) {
            return make_cancelled(env, n->line_num);
        }
    }

    int64_t max = engine_get_max_call_depth(eng);
    if (max <= 0) return NULL;

    callstack *cs = environment_callstack(env);
    int depth = (cs != NULL) ? callstack_get_depth(cs) : 0;
    if ((int64_t)depth + 1 <= max) return NULL;

    /* Refusing the call is the whole point: without this the next
     * frame would run the native stack out and the process would die
     * on SIGSEGV, taking the host with it. There is no C equivalent of
     * catching StackOverflowError after the fact. */
    char text[256];
    snprintf(text, sizeof(text),
             "Maximum call depth of %lld exceeded calling '%s'. "
             "Infinite recursion perhaps?", (long long)max, funct_name);
    char more[384];
    snprintf(more, sizeof(more),
             "%s Raise the security property '%s' if the recursion is intended.",
             text, ENGINE_LIMIT_CALL_DEPTH_PROP);

    av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
    char *trace = (cs != NULL) ? callstack_get_stack_trace(cs) : AUSSOM_STRDUP("");
    av_exception_set(e, n->line_num, ENGINE_CALL_DEPTH_EXCEEDED_ID, text, more, trace);
    return &e->base.base;
}

av_type *eval_stack_overflow(environment *env, int line)
{
    stack_guard_clear();
    callstack *cs = (env != NULL) ? environment_callstack(env) : NULL;
    char *trace = (cs != NULL) ? callstack_get_stack_trace(cs) : AUSSOM_STRDUP("");
    av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
    av_exception_set(e, line, ENGINE_STACK_OVERFLOW_ID,
        "The interpreter ran out of stack space while evaluating this program. "
        "Deeply nested expressions or data, or recursion the call depth "
        "limit could not see.",
        "Raise the thread stack size, simplify the expression or data being "
        "evaluated, or lower '" ENGINE_LIMIT_CALL_DEPTH_PROP "'.",
        trace);
    return &e->base.base;
}

char *eval_str(const av_type *v)
{
    stack_guard_clear();
    char *s = av_str(v);
    return stack_guard_tripped() ? NULL : s;
}

/* --- child chaining ---------------------------------------------- */

/* After a node produces `ret`, descend into its child (if any) with
 * the child's current-object set to `ret`. Mirrors the
 * `if (child != null) { tenv = env.clone(ret); return child.eval(...) }`
 * tail shared by most evalImpl methods. */
av_type *eval_child(av_type *ret, ast_node *n, environment *env, bool getref)
{
    if (n->child != NULL) {
        environment *tenv = environment_clone(env, ret);
        return ast_node_eval(n->child, tenv, getref);
    }
    return ret;
}

/* Evaluates a condition expression to a bool, propagating control
 * signals. Writes the bool result to *out_true; returns NULL on
 * normal flow or a control/exception value to propagate. */
av_type *eval_condition(ast_node *expr, environment *env, bool getref, bool *out_true)
{
    av_type *etmp = ast_node_eval(expr, env, getref);
    if (av_is_break_return_except(etmp)) return etmp;
    av_type *b = av_eval_expression_bool(etmp);
    if (av_is_ex(b)) return b;
    *out_true = av_bool_get((av_bool *)b);
    return NULL;
}

/* --- dispatcher -------------------------------------------------- */

/* The thread-local check, for a node whose frame did not settle it:
 * no frame, a frame made on another thread, or a stack that really is
 * low. Out of line so the common path stays two compares. */
__attribute__((noinline, cold))
static av_type *eval_stack_slow(ast_node *n, environment *env)
{
    if (!stack_guard_low()) return NULL;
    return eval_stack_overflow(env, n->line_num);
}

/* Evaluates `n` by its type. Inlined into both callers below, so the
 * path with no debugger is the same code it was before the debugger
 * hooks existed. */
static inline __attribute__((always_inline))
av_type *eval_by_type(ast_node *n, environment *env, bool getref)
{
    switch (n->type) {
    case AST_TYPE_NULL:           return ast_null_eval(n, env, getref);
    case AST_TYPE_BOOL:           return ast_bool_eval(n, env, getref);
    case AST_TYPE_INT:            return ast_int_eval(n, env, getref);
    case AST_TYPE_DOUBLE:         return ast_double_eval(n, env, getref);
    case AST_TYPE_STRING:         return ast_string_eval(n, env, getref);
    case AST_TYPE_LIST:           return ast_list_eval(n, env, getref);
    case AST_TYPE_MAP:            return ast_map_eval(n, env, getref);
    case AST_TYPE_VAR:            return ast_var_eval(n, env);
    case AST_TYPE_OBJ:            return ast_obj_eval(n, env, getref);
    case AST_TYPE_EXP:            return ast_expression_eval((ast_expression *)n, env, getref);
    case AST_TYPE_STATEMENTLIST:  return ast_statement_list_eval(n, env, getref);
    case AST_TYPE_RETURN:         return ast_return_eval(n, env, getref);
    case AST_TYPE_BREAK:          return ast_break_eval(n, env, getref);
    case AST_TYPE_IFELSE:         return ast_ifelse_eval(n, env, getref);
    case AST_TYPE_CONDITION:      return ast_condition_block_eval((ast_condition_block *)n, env, getref);
    case AST_TYPE_SWITCH:         return ast_switch_eval(n, env, getref);
    case AST_TYPE_WHILE:          return ast_while_eval(n, env, getref);
    case AST_TYPE_FOR:            return ast_for_eval(n, env, getref);
    case AST_TYPE_THROW:          return ast_throw_eval(n, env, getref);
    case AST_TYPE_TRYCATCH:       return ast_trycatch_eval(n, env, getref);
    case AST_TYPE_INCLUDE:        return ast_include_eval(n, env, getref);
    case AST_TYPE_FUNCTCALL:      return ast_funct_call_eval(n, env, getref);
    case AST_TYPE_NEWINST:        return ast_new_inst_eval(n, env, getref);
    case AST_TYPE_CALLBACK:       return ast_callback_eval(n, env, getref);
    /* Not directly evaluable as standalone nodes. */
    case AST_TYPE_FUNCTDEFARGSLIST:
    case AST_TYPE_FUNCTDEF:
    case AST_TYPE_CLASS:
    case AST_TYPE_ETCETERA:
    case AST_TYPE_AUSSOM_DOC:
    case AST_TYPE_ANNOTATION:
    case AST_TYPE_ANNOTATION_ARG:
    case AST_TYPE_UNDEF:
        break;
    }
    return eval_make_exception(env, n->line_num, "NOT_IMPLEMENTED",
        "Evaluation not implemented for this node type yet.");
}

/* astNode.eval's slow path, taken only while a debugger is installed.
 * Before the node: a set breakpoint flag pauses with BREAKPOINT,
 * otherwise should_pause_for_step may pause with STEP. After it: the
 * first time an exception value flows out, on_exception hears of it,
 * once, deduplicated by the value's debugger_seen flag.
 *
 * Java's hooks abort by throwing aussomException. Here a hook returns
 * false, and the node's result becomes the cancellation exception,
 * which unwinds the run the way engine_cancel does. */
__attribute__((noinline))
static av_type *eval_debug(ast_node *n, environment *env, bool getref)
{
    engine *eng = env->eng;
    const aussom_debugger *dbg = eng->debugger;
    if (dbg != NULL) {
        bool go = true;
        if (n->breakpoint) {
            go = dbg->on_pause(dbg->data, n, env, AUSSOM_PAUSE_BREAKPOINT);
        } else if (dbg->should_pause_for_step(dbg->data, n, env)) {
            go = dbg->on_pause(dbg->data, n, env, AUSSOM_PAUSE_STEP);
        }
        if (!go) return make_cancelled(env, n->line_num);
    }

    av_type *ret = eval_by_type(n, env, getref);

    if (ret != NULL && av_is_ex(ret)) {
        av_exception *ex = (av_exception *)ret;
        if (!av_exception_is_debugger_seen(ex)) {
            av_exception_set_debugger_seen(ex, true);
            dbg = eng->debugger;
            if (dbg != NULL && !dbg->on_exception(dbg->data, ex, env)) {
                return make_cancelled(env, n->line_num);
            }
        }
    }
    return ret;
}

/* Every node evaluation passes through here, which makes it the place
 * to stop before the native stack runs out (see stack_guard.h). This
 * covers nesting in code. A value walker that runs low instead leaves
 * the thread tripped, and its caller (the extern bridge, or an
 * evaluator that rendered a value) turns that into the exception.
 *
 * The bounds come from the current call frame rather than thread-local
 * storage; see stack_guard.h for why and for the other-thread case.
 *
 * It is also the debugger's hook site, as astNode.eval is in Java. With
 * no debugger installed that costs one branch on the engine's
 * debug_mode flag. */
av_type *ast_node_eval(ast_node *n, environment *env, bool getref)
{
    const callstack *cs = (env != NULL) ? env->st : NULL;
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (__builtin_expect(cs == NULL || here < cs->stack_limit
            || here >= cs->stack_high, 0)) {
        av_type *ov = eval_stack_slow(n, env);
        if (ov != NULL) return ov;
    }
    if (__builtin_expect(env != NULL && env->eng != NULL && env->eng->debug_mode, 0)) {
        return eval_debug(n, env, getref);
    }
    return eval_by_type(n, env, getref);
}
