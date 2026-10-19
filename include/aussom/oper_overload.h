/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_OPER_OVERLOAD_H
#define AUSSOM_OPER_OVERLOAD_H

#include <stdbool.h>

#include "aussom/ast_node.h"
#include "aussom/av_list.h"
#include "aussom/av_object.h"
#include "aussom/av_type.h"
#include "aussom/environment.h"
#include "aussom/exp_type.h"

/* Central helper for operator overloading (= com.aussom.ast.OperOverload).
 * Maps operator expression types to their reserved "dunder" method
 * names, answers whether a value's class defines one, and routes the
 * call through the standard class overload resolver.
 *
 * A class opts into an operator by declaring a plain method with the
 * well-known name; nothing else about dispatch, inheritance, mocking or
 * docgen changes. */

/* Unary and dereference operator method names used by callers outside
 * the binary method table. */
#define OPER_OP_NOT       "__opNot__"
#define OPER_OP_COUNT     "__opCount__"
#define OPER_OP_SET       "__opSet__"
#define OPER_OP_INDEX     "__opIndex__"
#define OPER_OP_INDEX_SET "__opIndexSet__"
#define OPER_OP_EQ        "__opEq__"
#define OPER_OP_NOT_EQ    "__opNotEq__"

/* The method called on the LEFT operand for a binary expression type,
 * or NULL when the type has no overloadable method. */
const char *oper_binary_method(exp_type t);

/* The method called on the RIGHT operand when the left does not
 * overload. Arithmetic and bitwise use dedicated __opRight*__ names;
 * comparisons mirror instead (2 < vec calls vec.__opGt__(2)). NULL when
 * no right-side method exists. */
const char *oper_reflected_method(exp_type t);

/* The source glyph for an expression type, used in error messages and
 * stack-trace frames so failures name the operator as written. */
const char *oper_symbol(exp_type t);

/* True when `val` is a class instance whose class defines `name`.
 * Primitives have a non-object c_type and exit on the first check, so
 * the hot expression path only pays a tag compare. */
bool oper_has_op(environment *env, const av_type *val, const char *name);

/* Calls an operator method on `target`. The environment is cloned onto
 * the target the way member calls are, so the body runs with normal
 * `this` access, and a call-site frame is pushed first so stack traces
 * point at the line where the operator was used. Resolution failures
 * are reworded to name the operator glyph rather than reading like a
 * plain method call. */
av_type *oper_call(environment *env, ast_node *site, const char *glyph,
                   av_object *target, const char *name, av_list *args);

/* Convenience wrapper for a single-argument operator method. */
av_type *oper_call_one(environment *env, ast_node *site, const char *glyph,
                       av_object *target, const char *name, av_type *arg);

#endif /* AUSSOM_OPER_OVERLOAD_H */
