/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_EXCEPTION_H
#define AUSSOM_AV_EXCEPTION_H

#include <stdbool.h>

#include "aussom/av_object.h"

/* Mirrors com.aussom.types.AussomException.exType. */
typedef enum av_ex_type {
    AV_EX_UNDEF = 0,
    AV_EX_INTERNAL,
    AV_EX_RUNTIME
} av_ex_type;

const char *av_ex_type_name(av_ex_type t);

/* User-code exception value (cException). Distinct from the
 * engine-level aussom_exception (a parser/dispatcher error). Flows
 * through evaluation as a value; av_is_ex treats it as "in flight"
 * unless local_object is set (i.e. it has been caught and is being
 * inspected). */
typedef struct av_exception {
    av_object base;
    av_ex_type et;
    int line_number;
    char *id;
    char *text;
    char *details;
    char *stack_trace;
    bool local_object;
    bool debugger_seen;
    /* True only for the exception raised by engine_cancel. A
     * cancellation is the host's decision rather than a fault in the
     * program, so try/catch re-raises it instead of catching it. */
    bool cancellation;
    /* Set by av_exception_new_thrown until class_call_closure adds the
     * wrapper text that names the method. */
    bool thrown;
} av_exception;

av_exception *av_exception_new(const char *text);
av_exception *av_exception_new_type(av_ex_type et);

/* For a C extern method to return where the Java method throws, rather
 * than returns, its exception. astFunctDef.callExtern reports a thrown
 * exception as EXTERN_INVOCATION_TARGET_EXCEPTION with the text
 * "External call, invocation target exception for method '<name>', the
 * external method threw an uncaught exception: <thrown>". The C method
 * passes <thrown>, the Java exception's toString(), and
 * class_call_closure adds the rest once it knows the method's name.
 *
 * av_exception_new_thrown_aussom is for a thrown aussomException,
 * whose toString() is "com.aussom.ast.aussomException: <message>". For
 * a JVM exception (StringIndexOutOfBoundsException and the like) the
 * text after the class name only needs to say the same thing as Java's. */
av_exception *av_exception_new_thrown(const char *thrown);
av_exception *av_exception_new_thrown_aussom(const char *message);

/* setException(line, id, text, details, stack). Strings are copied. */
void av_exception_set(av_exception *e, int line, const char *id,
                      const char *text, const char *details,
                      const char *stack_trace);

av_ex_type  av_exception_get_et(const av_exception *e);
int         av_exception_get_line(const av_exception *e);
const char *av_exception_get_id(const av_exception *e);
const char *av_exception_get_text(const av_exception *e);
const char *av_exception_get_details(const av_exception *e);
const char *av_exception_get_stack_trace(const av_exception *e);

void av_exception_set_id(av_exception *e, const char *id);
void av_exception_set_line(av_exception *e, int line);
void av_exception_set_stack_trace(av_exception *e, const char *stack_trace);

bool av_exception_is_local_object(const av_exception *e);
void av_exception_set_local_object(av_exception *e, bool v);
bool av_exception_is_debugger_seen(const av_exception *e);
void av_exception_set_debugger_seen(av_exception *e, bool v);
bool av_exception_is_cancellation(const av_exception *e);
void av_exception_set_cancellation(av_exception *e, bool v);

#endif /* AUSSOM_AV_EXCEPTION_H */
