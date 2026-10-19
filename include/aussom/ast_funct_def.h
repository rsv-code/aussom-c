/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_FUNCT_DEF_H
#define AUSSOM_AST_FUNCT_DEF_H

#include <stdbool.h>

#include "aussom/ast_funct_def_args_list.h"
#include "aussom/ast_node.h"
#include "aussom/ast_statement_list.h"

/* Mirrors com.aussom.ast.astFunctDef. Holds the parsed parameter
 * list and body for one declared function (or one overload).
 * Dispatcher / overload-resolution state (signature mangling,
 * arity calculation, ambiguity checks) lives at the runtime layer
 * and is not part of this AST port. */
typedef struct ast_funct_def {
    ast_node base;
    ast_funct_def_args_list *arg_list;
    ast_statement_list *instruction_list;
    bool is_extern;
    /* True when this definition was written as a closure expression
     * (::name(args) { ... }) inside another function body and hoisted
     * onto the class at parse time. Drives the parent-locals copy in
     * class_call, the ::name capture check in ast_callback_eval, and
     * the isClosure flag in doc generation. */
    bool is_closure;
} ast_funct_def;

ast_funct_def *ast_funct_def_new(void);
ast_funct_def *ast_funct_def_new_named(const char *name);

void ast_funct_def_set_arg_list(ast_funct_def *n, ast_funct_def_args_list *args);
ast_funct_def_args_list *ast_funct_def_get_arg_list(const ast_funct_def *n);

void ast_funct_def_set_instruction_list(ast_funct_def *n, ast_statement_list *body);
ast_statement_list *ast_funct_def_get_instruction_list(const ast_funct_def *n);

void ast_funct_def_set_extern(ast_funct_def *n, bool is_extern);
bool ast_funct_def_get_extern(const ast_funct_def *n);

void ast_funct_def_set_closure(ast_funct_def *n, bool is_closure);
bool ast_funct_def_get_closure(const ast_funct_def *n);

/* --- overload-resolution helpers (mirror astFunctDef) ------------
 * These are pure functions of the declared arg list, used by the
 * class runtime to match a call's runtime arg types against an
 * overload. */

/* astFunctDef.effectiveArgType: the c_type a declared arg slot
 * accepts. ETCETERA -> cUndef (checked separately); the slot's declared
 * prim_type if set, even with a null default; otherwise inferred from
 * the node kind (VAR and an untyped null default -> cUndef wildcard). */
c_type ast_funct_def_effective_arg_type(const ast_node *arg);

/* astFunctDef.mangleChar: one letter per type in a signature. */
char ast_funct_def_mangle_char(c_type t);

/* astFunctDef.getSignature: the declared slot types, "i,*" or
 * "s,...", "" for none. Returns a GC string. */
char *ast_funct_def_signature(const ast_funct_def *n);

/* astFunctDef.hasWildcard: true when a slot other than ETCETERA has
 * the effective type cUndef. */
bool ast_funct_def_has_wildcard(const ast_funct_def *n);

/* True when the last declared arg is ETCETERA (variadic). */
bool ast_funct_def_is_variadic(const ast_funct_def *n);

/* Number of leading required (untyped/typed VAR, no default) slots
 * before the first default-bearing arg or ETCETERA. */
int ast_funct_def_min_arity(const ast_funct_def *n);

/* Total declared slots, or INT_MAX when variadic. */
int ast_funct_def_max_arity(const ast_funct_def *n);

#endif /* AUSSOM_AST_FUNCT_DEF_H */
