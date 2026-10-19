/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Console logger. Ports com.aussom.LoggingInt and
 * com.aussom.DefaultLoggingImpl: an installable logger interface plus
 * the stdout implementation installed by default. The leveled methods
 * (trc/dbg/info/warn/err) prefix the message with its priority tag
 * ("[trace] ", "[debug] ", "[info] ", "[warning] ", "[error] ") and are
 * suppressed when below the current level; log/print/println are
 * unprefixed. All output goes to stdout, matching aussom-base. The `c`
 * stdlib class and the C-side driver (test runner, CLI) both route
 * through this so their output matches the Java implementation exactly.
 *
 * These free functions write through a PROCESS-WIDE logger, and exist
 * for the paths that run before or outside any engine -- the CLI's own
 * argument errors, for one. Anything with an engine in scope should use
 * the per-engine writers in engine.h (engine_err, engine_println, ...)
 * instead: those port Engine.getLogger/setLogger, so output belongs to
 * the engine that produced it rather than to whatever was installed
 * process-wide, and two engines never cross-route.
 */

#ifndef AUSSOM_CONSOLE_H
#define AUSSOM_CONSOLE_H

typedef enum console_level {
    CONSOLE_TRACE   = 0,
    CONSOLE_DEBUG   = 1,
    CONSOLE_INFO    = 2,
    CONSOLE_WARNING = 3,
    CONSOLE_ERROR   = 4,
} console_level;

/* The installed logger. Ports com.aussom.LoggingInt: the same eight
 * methods, in the same order. Every field must be set; there is no
 * partial implementation.
 *
 * A logger installed here owns its own level gating, exactly as in
 * Java, where setLevel lives on DefaultLoggingImpl and not on
 * LoggingInt. console_set_level therefore configures the default
 * implementation only and does not reach a replacement. */
typedef struct console_logger {
    void (*log)(const char *s);
    void (*trc)(const char *s);
    void (*dbg)(const char *s);
    void (*info)(const char *s);
    void (*warn)(const char *s);
    void (*err)(const char *s);
    void (*print)(const char *s);
    void (*println)(const char *s);
} console_logger;

/* Installs a logger. Ports Engine.setLogger: passing NULL restores the
 * default stdout implementation, and the pointer is borrowed, so the
 * caller keeps it alive for as long as it is installed. Hosts whose
 * stdout goes nowhere (an Android application, for one) install their
 * own here. */
void console_set_logger(const console_logger *logger);

/* The installed logger, never NULL. Ports Engine.getLogger. */
const console_logger *console_get_logger(void);

/* The default stdout implementation, so a host can chain to it. */
const console_logger *console_default_logger(void);

/* The default implementation defaults to CONSOLE_INFO (trace/debug
 * suppressed). Ports DefaultLoggingImpl.setLevel, so it configures the
 * default implementation and not a replacement. */
void          console_set_level(console_level level);
console_level console_get_level(void);

/* Unprefixed. log is gated at INFO; print/println always emit. */
void console_log(const char *s);
void console_print(const char *s);
void console_println(const char *s);

/* Leveled: prefixed with the priority tag, gated by the current level. */
void console_trc(const char *s);
void console_dbg(const char *s);
void console_info(const char *s);
void console_warn(const char *s);
void console_err(const char *s);

#endif /* AUSSOM_CONSOLE_H */
