/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_DEBUGGER_H
#define AUSSOM_DEBUGGER_H

#include <stdbool.h>

struct ast_node;
struct environment;
struct av_exception;

/* Why the interpreter is calling on_pause. Ports com.aussom.PauseReason.
 * These are the two pre-eval pause paths the engine knows about; a
 * debugger that tracks richer intent (an external pause request, a step
 * that reached its target) does so itself. */
typedef enum aussom_pause_reason {
    /* The node's breakpoint flag was set. */
    AUSSOM_PAUSE_BREAKPOINT,
    /* should_pause_for_step returned true. */
    AUSSOM_PAUSE_STEP
} aussom_pause_reason;

/* What an external debugger implements to drive an interpreter through
 * breakpoints, stepping and exception inspection. Ports
 * com.aussom.DebuggerInt as a table of function pointers; `data` is
 * handed back to every call.
 *
 * Installed with engine_set_debugger. The interpreter calls it from one
 * place, the top of ast_node_eval, and only while a debugger is
 * installed. Step modes are the debugger's business: the engine only
 * asks should_pause_for_step and reports which path fired.
 *
 * Java's DebuggerInt aborts a run by throwing aussomException out of
 * onPause or onException. C has no exceptions, so here a call returns
 * false to stop the run instead. The engine then raises the same
 * cancellation exception engine_cancel does, which a try/catch in the
 * script cannot swallow. */
typedef struct aussom_debugger {
    void *data;

    /* Called when the interpreter has decided to pause before `node`.
     * Expected to block the calling thread until the debugger lets it
     * go. From `env` the debugger can reach the call stack, the locals
     * and `this`. Returns true to continue, false to stop the run. */
    bool (*on_pause)(void *data, struct ast_node *node,
                     struct environment *env, aussom_pause_reason reason);

    /* Asked before every node whose breakpoint flag is clear. Returns
     * true to pause with AUSSOM_PAUSE_STEP. Hot path: keep it cheap. */
    bool (*should_pause_for_step)(void *data, struct ast_node *node,
                                  struct environment *env);

    /* Called the first time an exception value flows out of a node,
     * once per exception rather than once per frame it passes through
     * (deduplicated by the value's debugger_seen flag). May block to
     * pause on the throw. Returns true to let the exception continue,
     * false to stop the run.
     *
     * Java also has onException(Exception, Environment) for a thrown
     * Java exception. C errors are values only, so this one form covers
     * both. */
    bool (*on_exception)(void *data, struct av_exception *ex,
                         struct environment *env);
} aussom_debugger;

#endif /* AUSSOM_DEBUGGER_H */
