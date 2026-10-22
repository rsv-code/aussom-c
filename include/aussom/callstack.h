/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_CALLSTACK_H
#define AUSSOM_CALLSTACK_H

#include <stdint.h>
#include <stdio.h>

/* Forward decl; a frame may name the function it represents. */
struct ast_funct_def;
struct av_strmap;

/* Mirrors com.aussom.CallStack: a singly linked chain of call frames
 * used for stack traces and the debugger. Each interpreter
 * thread owns its own chain, so unlike the Java original these
 * accessors are not individually locked -- the chain is not shared
 * across threads in the eval model. GC-allocated. */
/* The string fields are borrowed pointers, not owned copies: every
 * caller passes AST-owned strings (file/class/function names), string
 * literals, or other GC-managed strings that outlive the frame. The
 * GC-scanned struct keeps any GC string alive while the frame
 * references it, so no strdup is needed (a frame is read-only, used
 * only to format a stack trace). */
typedef struct callstack {
    struct callstack *parent;
    const char *file_name;
    int line_number;
    const char *class_name;
    const char *function_name;
    const char *text;
    struct ast_funct_def *called_function;

    /* The locals of the call this frame stands for, or NULL when it has
     * none of its own. Ports CallStack.locals: set on a function body's
     * "Defined." frame, so a debugger paused deeper down can still show
     * what each frame below it holds. */
    struct av_strmap *locals;

    /* How many Aussom function calls are active on this chain, with
     * the root frame at 0. Recorded when the frame is parented so
     * reading it is a field read rather than a walk up the chain. The
     * interpreter checks it once per call against the engine's call
     * depth limit; see eval_check_call_gate.
     *
     * Counted in calls, not frames. As in aussom-base, one Aussom call
     * produces more than one frame: the call site pushes "Function
     * called." (or "Constructor called." / "Operator '+' called.") and
     * the body pushes "Defined.", and there are synthetic frames for
     * argument defaults, member initializers and static initializers.
     * Only the body frame calls callstack_enter_call, so a limit of 50
     * means 50 nested Aussom calls rather than some multiple of it that
     * would depend on how the frames happen to be laid out. */
    int depth;

    /* The native stack bounds of the thread that made this frame,
     * copied from stack_guard_bounds so the per-node stack check in
     * ast_node_eval reads two fields instead of thread-local storage.
     * See stack_guard.h. */
    uintptr_t stack_limit;
    uintptr_t stack_high;
} callstack;

callstack *callstack_new(void);
callstack *callstack_new_frame(const char *file_name, int line_number,
                               const char *class_name,
                               const char *function_name, const char *text);

void        callstack_set_parent(callstack *cs, callstack *parent);
callstack  *callstack_get_parent(const callstack *cs);

/* Records that this frame is the body of an Aussom function call, one
 * level deeper than its parent. Called after the frame is parented,
 * and by nothing else: it is what makes the depth count calls rather
 * than frames. */
void callstack_enter_call(callstack *cs);

/* How many Aussom function calls are active on this chain, root at 0.
 * Recorded as frames are pushed rather than counted on demand, so the
 * call depth check stays a field read. */
int callstack_get_depth(const callstack *cs);

void callstack_set_called_function(callstack *cs, struct ast_funct_def *f);
struct ast_funct_def *callstack_get_called_function(const callstack *cs);

void callstack_set_locals(callstack *cs, struct av_strmap *locals);
struct av_strmap *callstack_get_locals(const callstack *cs);

/* Builds the multi-line trace from this frame up through its parents.
 * Returns a GC-managed string. */
char *callstack_get_stack_trace(const callstack *cs);

#endif /* AUSSOM_CALLSTACK_H */
