/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Evaluation declarations. Mirrors aussom-base, where each AST node
 * carries its own evalImpl: here each node's eval body lives in its
 * src/ast/ast_<name>.c file as ast_<name>_eval, and the base
 * ast_node_eval (in ast_node.c) dispatches on the node tag -- the same
 * shape as the ast_<name>_write_body / ast_node_to_string pair.
 *
 * Also declares the class/function/callback runtime entry points
 * (astClass.instantiate/call, astFunctDef.call, AussomCallback.call)
 * and the few shared eval helpers.
 */

#ifndef AUSSOM_AST_EVAL_H
#define AUSSOM_AST_EVAL_H

#include <stdbool.h>

#include "aussom/ast_class.h"
#include "aussom/ast_condition_block.h"
#include "aussom/ast_expression.h"
#include "aussom/ast_funct_def.h"
#include "aussom/ast_funct_def_args_list.h"
#include "aussom/ast_node.h"
#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/av_callback.h"
#include "aussom/av_list.h"
#include "aussom/av_type.h"
#include "aussom/engine.h"
#include "aussom/environment.h"

/* Base dispatcher (= astNode.eval). Evaluates `n` in `env`; with
 * getref true an lvalue node yields an av_ref. Always non-NULL. */
av_type *ast_node_eval(ast_node *n, environment *env, bool getref);

/* Per-node eval bodies (= each subclass's evalImpl). */
av_type *ast_null_eval(ast_node *n, environment *env, bool getref);
av_type *ast_bool_eval(ast_node *n, environment *env, bool getref);
av_type *ast_int_eval(ast_node *n, environment *env, bool getref);
av_type *ast_double_eval(ast_node *n, environment *env, bool getref);
av_type *ast_string_eval(ast_node *n, environment *env, bool getref);
av_type *ast_include_eval(ast_node *n, environment *env, bool getref);
av_type *ast_break_eval(ast_node *n, environment *env, bool getref);
av_type *ast_list_eval(ast_node *n, environment *env, bool getref);
av_type *ast_map_eval(ast_node *n, environment *env, bool getref);
av_type *ast_var_eval(ast_node *n, environment *env);
av_type *ast_obj_eval(ast_node *n, environment *env, bool getref);
av_type *ast_statement_list_eval(ast_node *n, environment *env, bool getref);
av_type *ast_return_eval(ast_node *n, environment *env, bool getref);
av_type *ast_condition_block_eval(ast_condition_block *cb, environment *env, bool getref);
av_type *ast_ifelse_eval(ast_node *n, environment *env, bool getref);
av_type *ast_switch_eval(ast_node *n, environment *env, bool getref);
av_type *ast_while_eval(ast_node *n, environment *env, bool getref);
av_type *ast_for_eval(ast_node *n, environment *env, bool getref);
av_type *ast_throw_eval(ast_node *n, environment *env, bool getref);
av_type *ast_trycatch_eval(ast_node *n, environment *env, bool getref);
av_type *ast_funct_call_eval(ast_node *n, environment *env, bool getref);
av_type *ast_new_inst_eval(ast_node *n, environment *env, bool getref);
av_type *ast_callback_eval(ast_node *n, environment *env, bool getref);
av_type *ast_expression_eval(ast_expression *e, environment *env, bool getref);

/* Interpreter checkpoint, called on every loop back edge. Reads the
 * engine's control state and decides whether the program may keep
 * going:
 *
 *   RUNNING   -- returns NULL, the fast path: one read and a compare.
 *   PAUSED    -- blocks here until the engine is resumed or cancelled,
 *                then answers for the new state.
 *   CANCELLED -- returns the cancellation exception to propagate.
 *
 * Callers treat a non-NULL result the way they treat any other
 * exception value: stop looping and hand it back.
 *
 * The exception carries ENGINE_CANCELLED_EXCEPTION_ID and has its
 * cancellation flag set, so a host can tell a stopped program from a
 * broken one, and try/catch can keep a catch block from swallowing it. */
av_type *eval_check_control(ast_node *n, environment *env);

/* The same checkpoint without an AST node, for stdlib code that has an
 * environment but no node to attribute the line to: the sleep slices
 * and the regex scan loops. Reports line 0. */
av_type *eval_check_control_env(environment *env);

/* Call-path gate, checked once per Aussom call from class_call_closure,
 * which every call reaches: direct calls, constructors, callbacks,
 * closures, operator overloads and extern dispatch. Two guards, in the
 * order a host cares about them: the engine's control state, then the
 * call depth.
 *
 * The depth limit is what turns runaway recursion from a SIGSEGV that
 * kills the process into an ordinary, catchable Aussom exception
 * reported at the call that went too deep. It reads the depth off the
 * caller's frame, which recorded it when the frame was parented, so the
 * check is a field read rather than a walk.
 *
 * Returns NULL to keep going, or the exception to propagate. */
av_type *eval_check_call_gate(ast_node *n, environment *env, const char *funct_name);

/* Builds the STACK_OVERFLOW exception and clears the thread's tripped
 * mark. Port of Engine.stackOverflowToException. Unlike Java, which
 * converts the overflow only at its public entry points, this is an
 * ordinary exception raised where the stack ran low, so a script's
 * try/catch sees it once the stack has unwound to the try. */
av_type *eval_stack_overflow(environment *env, int line);

/* av_str for the evaluator. Returns NULL when rendering the value ran
 * the native stack low (a container that holds itself, or data nested
 * too deeply); the caller returns eval_stack_overflow then, so the
 * partial text is never used. Clears any earlier tripped mark first, so
 * a mark left by host code that rendered a value cannot be mistaken
 * for this one. */
char *eval_str(const av_type *v);

/* Shared eval helpers (defined in ast_node.c). */
av_type *eval_make_exception(environment *env, int line, const char *id,
                             const char *text);
av_type *eval_child(av_type *ret, ast_node *n, environment *env, bool getref);
av_type *eval_condition(ast_node *expr, environment *env, bool getref, bool *out_true);

/* Control-signal predicates, mirroring the astNode statics. */
bool av_is_break_return_event(const av_type *v);
bool av_is_break_event(const av_type *v);
bool av_is_break_return_except(const av_type *v);

/* Call/constructor argument evaluation (= astFunctDefArgsList.eval).
 * Returns NULL and sets *out on success, or an exception. */
av_type *eval_args(ast_funct_def_args_list *args, environment *env, av_list **out);

/* Class / function / callback runtime (astClass.instantiate/call,
 * astFunctDef.call, AussomCallback.call). */
/* The class definition to dispatch `v`'s methods through.
 *
 * An instantiated object carries its own definition and that is what
 * comes back. A primitive carries none -- see av_object_init -- so its
 * definition is resolved from the calling engine, which is what keeps
 * two engines in one process from sharing one. Returns NULL for a
 * control carrier (ref/break/return), or when the engine has no
 * definition for the type.
 *
 * Ports the 25 dispatch-shaped getClassDef() sites in aussom-base,
 * which take the definition from env.getEngine() for the same reason.
 * Prefer this to av_object_get_class_def wherever the value could be a
 * primitive. */
ast_class *class_def_for(environment *env, av_type *v);

/* The value's type name as AussomObject.getTypeName gives it: "bool",
 * "int", "double", "string", "list", "map", "cnull", "callback" or
 * "exception" for a value type, the class name for an object. Used to
 * name the class in call stack frames, as Java does. */
const char *eval_type_name(const av_type *v);

av_type *class_call(environment *env, bool getref, ast_class *cls,
                    const char *name, av_list *args);

/* Dispatch with an explicit parent-locals argument for closure calls.
 * When the resolved overload is a closure, its fresh locals are seeded
 * with a shallow copy of `closure_parent_locals` (the scope captured on
 * the callback at the definition site), or of the invoker's env locals
 * when that is NULL (direct call by name). Non-closure targets ignore
 * it entirely, so class_call is just this with NULL. */
av_type *class_call_closure(environment *env, bool getref, ast_class *cls,
                            const char *name, av_list *args,
                            av_strmap *closure_parent_locals);
av_type *class_instantiate(environment *env, ast_class *cls, bool getref,
                           av_list *args);

/* The text of the INSTANTIATION_FAILURE exception Java's `new` reports
 * when building an object throws. */
#define CLASS_INSTANTIATION_THREW "Instantiation threw exception."

/* Builds that exception for an error Java throws while instantiating
 * `cls`; `msg` leads its stack trace. */
av_type *class_instantiate_throw(environment *env, ast_class *cls, const char *msg);

/* Builds the single instance of a static class. The only way past the
 * static check in class_instantiate; reserved for the startup pass over
 * the static class definitions. */
av_type *class_instantiate_static_singleton(environment *env, ast_class *cls,
                                            bool getref, av_list *args);

/* astFunctDef.initArgs: binds `args` (and evaluated defaults) into
 * env's locals. Default values evaluate in env, so the caller gives it
 * the "<arg-defaults>" frame Java uses. Returns an exception or a bool. */
av_type *funct_def_init_args(environment *env, ast_funct_def *fd, av_list *args);

/* Runs the body of `fd` in env, whose locals already hold the
 * arguments. Returns the return value or an exception. */
av_type *funct_def_run_body(environment *env, bool getref, ast_funct_def *fd);
av_type *callback_call(av_callback *cb, environment *env, av_list *args);

#endif /* AUSSOM_AST_EVAL_H */
