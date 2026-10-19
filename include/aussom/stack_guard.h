/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Native stack guard: the C stand-in for Java's StackOverflowError.
 *
 * The interpreter walks trees by recursion: ast_node_eval for code, and
 * the str/toString/toJson/pack/clone walkers for values. Each level
 * uses native stack. In Java, running out is a StackOverflowError the
 * engine converts into a STACK_OVERFLOW Aussom exception. In C the next
 * call touches the guard page and the process dies on SIGSEGV, with no
 * return value to check the way malloc returns NULL.
 *
 * So the check happens before the stack runs out. Each thread asks the
 * OS once where its stack ends (pthread_getattr_np on Linux and
 * Android, pthread_get_stackaddr_np on Apple, GetCurrentThreadStackLimits
 * on Windows). A check compares the current frame address with that end
 * and reports "low" once fewer than STACK_GUARD_RESERVE bytes remain.
 * There is no depth count: the limit is whatever stack the host gave
 * the thread.
 *
 * The reserve is room for the C code that runs between two checks
 * (one node's own work, printf, the collector, a regex match) and for
 * the exception being built and returned. The reserve was sized when
 * json.parse used cJSON, whose recursive parse of 999 nested arrays
 * needed 64 to 72 KB on x86-64 at -O2 (measured by shrinking the
 * reserve until a script that parses it at every level of a deep
 * recursion crashed). Frames are larger on Windows x64 (32 bytes of
 * shadow space per call) and usually on ARM64, so the reserve is about
 * 3.5 times that. json.parse now uses the json-simple port, which
 * parses without recursion and checks the stack at each level while it
 * builds the result. The stack overflow tests in tests/test_engine.c
 * still run that deep-JSON script on whatever platform they run on.
 *
 * Two ways a check is used:
 *
 *   - ast_node_eval checks on entry and returns the STACK_OVERFLOW
 *     exception itself. It runs for every node, and reaching a
 *     thread-local is a function call in position independent code and
 *     on Apple and Android, so it does not read the thread-local state.
 *     Each call frame (callstack) copies its thread's bounds when it is
 *     made, which happens once per Aussom call on the thread that runs
 *     it, and the per-node check compares against those. An address
 *     outside the frame's recorded range means the frame is being used
 *     on another thread; that case falls back to the thread-local check,
 *     so it is never mistaken for an overflow.
 *   - A value walker cannot return an exception (the str walkers write
 *     to a FILE *). When a check says "low" it also marks the thread as
 *     tripped. The walker stops and unwinds, and the next
 *     ast_node_eval to return converts the mark into the exception. A
 *     caller that acts on a walker's output (printing it, storing it)
 *     checks stack_guard_tripped() first so a partial result is never
 *     used.
 *
 * Stacks are assumed to grow downward, which holds on every platform
 * aussom-c targets. If the OS cannot report a thread's bounds the guard
 * stays off for that thread and behavior is what it was before.
 */

#ifndef AUSSOM_STACK_GUARD_H
#define AUSSOM_STACK_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/* Bytes kept free below the deepest check. */
#define STACK_GUARD_RESERVE (256 * 1024)

typedef struct stack_guard_state {
    /* Lowest address a check may see; 0 when the guard is off. */
    uintptr_t limit;
    /* The stack's top (its highest address); UINTPTR_MAX when the guard
     * is off, so [limit, high) then holds every address. */
    uintptr_t high;
    /* A check failed and the overflow has not been reported yet. */
    bool tripped;
    /* limit has been computed for this thread. */
    bool ready;
} stack_guard_state;

extern _Thread_local stack_guard_state stack_guard_tls;

/* Computes this thread's limit. Called once per thread by
 * stack_guard_low. */
void stack_guard_init_thread(stack_guard_state *s);

/* stack_guard_low for a caller that already holds this thread's state
 * (&stack_guard_tls), to avoid looking it up again. */
static inline bool stack_guard_low_at(stack_guard_state *s)
{
    if (!s->ready) stack_guard_init_thread(s);
    if ((uintptr_t)__builtin_frame_address(0) >= s->limit) return false;
    s->tripped = true;
    return true;
}

/* True when this thread is within STACK_GUARD_RESERVE bytes of the end
 * of its stack. Also marks the thread as tripped. */
static inline bool stack_guard_low(void)
{
    return stack_guard_low_at(&stack_guard_tls);
}

/* This thread's bounds, [limit, high), for a call frame to copy. */
static inline void stack_guard_bounds(uintptr_t *limit, uintptr_t *high)
{
    stack_guard_state *s = &stack_guard_tls;
    if (!s->ready) stack_guard_init_thread(s);
    *limit = s->limit;
    *high = s->high;
}

/* Marks the thread as tripped without a stack check, for a walker that
 * has proved it can never finish (a container reached through itself).
 * The result is the same STACK_OVERFLOW the stack would have reached. */
static inline void stack_guard_trip(void)
{
    stack_guard_tls.tripped = true;
}

/* True when a check failed and the overflow has not been reported. */
static inline bool stack_guard_tripped(void)
{
    return stack_guard_tls.tripped;
}

/* Clears the tripped mark, once the overflow has become an exception. */
static inline void stack_guard_clear(void)
{
    stack_guard_tls.tripped = false;
}

#endif /* AUSSOM_STACK_GUARD_H */
