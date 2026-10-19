/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The process-wide console logger (com.aussom.LoggingInt and its
 * default implementation com.aussom.DefaultLoggingImpl) and the `c`
 * static class (com.aussom.stdlib.console) that exposes it to
 * Aussom code. Leveled methods (trc/dbg/info/warn/err) prefix the
 * message with its priority tag and are gated by the current level;
 * log/print/println are unprefixed. Everything goes to stdout, matching
 * aussom-base. Each `c` method returns the static console object so
 * calls chain.
 */

#include <stdio.h>

#include "aussom/av.h"
#include "aussom/console.h"
#include "aussom/engine.h"
#include "aussom/extern_registry.h"
#include "aussom/stack_guard.h"
#include "stdlib_internal.h"

/* --- logger ------------------------------------------------------ */

/* Level for the default implementation only. A replacement logger owns
 * its own gating, exactly as in Java where setLevel lives on
 * DefaultLoggingImpl rather than on LoggingInt. */
static console_level g_level = CONSOLE_INFO;

void console_set_level(console_level level) { g_level = level; }
console_level console_get_level(void) { return g_level; }

/* --- the default implementation (com.aussom.DefaultLoggingImpl) --- */

static void emit(console_level at, const char *prefix, const char *s)
{
    if (g_level > at) return;
    if (prefix != NULL) fputs(prefix, stdout);
    av_text_write_out(s, stdout);
    fputc('\n', stdout);
}

static void dflt_log(const char *s)     { emit(CONSOLE_INFO, NULL, s); }
static void dflt_trc(const char *s)     { emit(CONSOLE_TRACE,   "[trace] ", s); }
static void dflt_dbg(const char *s)     { emit(CONSOLE_DEBUG,   "[debug] ", s); }
static void dflt_info(const char *s)    { emit(CONSOLE_INFO,    "[info] ", s); }
static void dflt_warn(const char *s)    { emit(CONSOLE_WARNING, "[warning] ", s); }
static void dflt_err(const char *s)     { emit(CONSOLE_ERROR,   "[error] ", s); }
static void dflt_print(const char *s)   { av_text_write_out(s, stdout); }
static void dflt_println(const char *s) { av_text_write_out(s, stdout); fputc('\n', stdout); }

static const console_logger g_default_logger = {
    dflt_log, dflt_trc, dflt_dbg, dflt_info,
    dflt_warn, dflt_err, dflt_print, dflt_println,
};

/* Never NULL. Ports the Java field initializer
 * `LoggingInt logger = new DefaultLoggingImpl()`. */
static const console_logger *g_logger = &g_default_logger;

void console_set_logger(const console_logger *logger)
{
    g_logger = (logger != NULL) ? logger : &g_default_logger;
}

const console_logger *console_get_logger(void) { return g_logger; }
const console_logger *console_default_logger(void) { return &g_default_logger; }

/* --- the published entry points, routed through the logger -------- */

void console_log(const char *s)     { g_logger->log(s); }
void console_trc(const char *s)     { g_logger->trc(s); }
void console_dbg(const char *s)     { g_logger->dbg(s); }
void console_info(const char *s)    { g_logger->info(s); }
void console_warn(const char *s)    { g_logger->warn(s); }
void console_err(const char *s)     { g_logger->err(s); }
void console_print(const char *s)   { g_logger->print(s); }
void console_println(const char *s) { g_logger->println(s); }

/* --- per-engine writers ------------------------------------------
 *
 * These live here rather than in engine.c because everything they need
 * to know about formatting and gating is here. An engine that has been
 * given a logger delegates to it unconditionally: a replacement owns
 * its own gating, matching Java, where setLevel is on
 * DefaultLoggingImpl and not on LoggingInt. An engine still on the
 * built-in default is gated against ITS OWN level rather than the
 * process-wide one, which is what lets two engines share the default
 * implementation at different verbosities. */

static void write_default(console_level threshold, console_level at,
                          const char *prefix, const char *s)
{
    if (threshold > at) return;
    if (prefix != NULL) fputs(prefix, stdout);
    av_text_write_out(s, stdout);
    fputc('\n', stdout);
}

void engine_log(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->log(s); return; }
    write_default(engine_get_log_level(e), CONSOLE_INFO, NULL, s);
}

void engine_trc(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->trc(s); return; }
    write_default(engine_get_log_level(e), CONSOLE_TRACE, "[trace] ", s);
}

void engine_dbg(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->dbg(s); return; }
    write_default(engine_get_log_level(e), CONSOLE_DEBUG, "[debug] ", s);
}

void engine_info(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->info(s); return; }
    write_default(engine_get_log_level(e), CONSOLE_INFO, "[info] ", s);
}

void engine_warn(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->warn(s); return; }
    write_default(engine_get_log_level(e), CONSOLE_WARNING, "[warning] ", s);
}

void engine_err(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->err(s); return; }
    write_default(engine_get_log_level(e), CONSOLE_ERROR, "[error] ", s);
}

/* print/println are never gated, matching the free functions. */
void engine_print(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->print(s); return; }
    av_text_write_out(s, stdout);
}

void engine_println(engine *e, const char *s)
{
    const console_logger *lg = engine_get_logger(e);
    if (lg != NULL) { lg->println(s); return; }
    av_text_write_out(s, stdout);
    fputc('\n', stdout);
}

/* --- `c` static class -------------------------------------------- */

/* NULL when rendering the argument ran the native stack low (a value
 * that holds itself, or one nested too deeply). Nothing is printed
 * then; the evaluator turns the tripped thread into STACK_OVERFLOW. */
static const char *arg_str(av_list *args)
{
    if (av_list_size(args) == 0) return "";
    const char *s = av_str(av_list_get(args, 0));
    return stack_guard_tripped() ? NULL : s;
}

static av_type *c_log(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_log(environment_engine(env), s);
    return self;
}

static av_type *c_trc(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_trc(environment_engine(env), s);
    return self;
}

static av_type *c_dbg(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_dbg(environment_engine(env), s);
    return self;
}

static av_type *c_info(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_info(environment_engine(env), s);
    return self;
}

static av_type *c_warn(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_warn(environment_engine(env), s);
    return self;
}

static av_type *c_err(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_err(environment_engine(env), s);
    return self;
}

static av_type *c_print(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_print(environment_engine(env), s);
    return self;
}

static av_type *c_println(av_type *self, environment *env, av_list *args)
{
    const char *s = arg_str(args);
    if (s != NULL) engine_println(environment_engine(env), s);
    return self;
}

const aussom_extern_method stdlib_console_methods[] = {
    { "log", c_log },
    { "trc", c_trc },
    { "dbg", c_dbg },
    { "info", c_info },
    { "warn", c_warn },
    { "err", c_err },
    { "print", c_print },
    { "println", c_println },
    { NULL, NULL }
};
