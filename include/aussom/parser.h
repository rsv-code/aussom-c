/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_PARSER_H
#define AUSSOM_PARSER_H

#include <stdbool.h>
#include <stdio.h>

#include "aussom/ast_statement_list.h"
#include "aussom/engine.h"

/* Parse failures come in the two kinds aussom-base has:
 *
 * - A syntax or lexer error is logged through the engine's logger,
 *   recorded as a diagnostic, and sets the engine's parse-error flag.
 *   A syntax error stops the parse at the first one. engine_run then
 *   refuses to run ("Parse errors were encountered").
 * - An error aussom-base throws out of the parse (an extern function
 *   that is not public, a number literal out of range, a duplicate
 *   overload, an extern class that is not found or not permitted, an
 *   include that fails, a static class whose constructor fails, a file
 *   that can't be read or is over the source byte limit) stops the
 *   parse and is NOT logged, recorded or flagged. It is left for the
 *   caller in engine_parse_exception, and the caller reports it, as a
 *   Java caller catches the exception. Print
 *   engine_parse_exception_stack_trace to show it the way the CLI
 *   does.
 *
 * Both return false. Classes read before the error stay in the
 * engine. */

/* Parses a source file, appending classes and includes to `eng`.
 * Returns true on a clean parse. */
bool aussom_parse_file(engine *eng, const char *file_path);

/* Parses a source string. `file_name` is used purely for error
 * messages and for the file_name field on every produced AST node;
 * it can be a logical name like "<repl>" when no on-disk file
 * exists. */
bool aussom_parse_string(engine *eng, const char *source, const char *file_name);

/* Parses `source` with top-level statements appended to `target`.
 * Ports Engine.parseStatements, the building block under
 * engine_parse_script_line and engine_eval_in_frame. It has no
 * security gate of its own; its callers check.
 *
 * line_offset is added to every line the scanner reports, so the first
 * source line reports as line_offset + 1. Closures defined in top-level
 * statements are added to the script class (engine_get_script_class);
 * with script mode never turned on there is none, and that is a parse
 * error.
 *
 * Clears the engine's parse diagnostics first, as Java does, so they
 * describe only this parse. An error the parse throws is turned into an
 * ordinary parse error: logged, recorded as a file-level diagnostic and
 * flagged. Returns true on a clean parse. */
bool aussom_parse_statements(engine *eng, const char *file_name,
                             const char *source, int line_offset,
                             ast_statement_list *target);

#endif /* AUSSOM_PARSER_H */
