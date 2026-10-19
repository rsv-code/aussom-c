/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for the host-facing engine API: engine_run (the port of
 * Engine.run) and the include search paths (the port of
 * Engine.addIncludePath and Engine.addExcludePath). These are what an
 * embedder calls, so they are tested the way an embedder would use
 * them rather than through the command line.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/console.h"
#include "aussom/av.h"
#include "aussom/av_object.h"
#include "aussom/av_strmap.h"
#include "aussom/ast_statement_list.h"
#include "aussom/engine.h"
#include "aussom/security_manager.h"
#include "aussom/parser.h"
#include "greatest.h"

/* Several cases below run programs that fail on purpose, and a failing
 * program logs through its engine's logger. Swallow that so the
 * expected failures do not look like test output. Installed per engine
 * (engine_set_logger), not process-wide: engine output no longer goes
 * through the process logger at all. */
static void quiet(const char *s) { (void)s; }
static const console_logger silent = {
    quiet, quiet, quiet, quiet, quiet, quiet, quiet, quiet,
};

static engine *engine_with(const char *source)
{
    engine *e = engine_new();
    if (!aussom_parse_string(e, source, "<test>")) return NULL;
    return e;
}

/* Same, with this engine's output swallowed for the whole of its life,
 * including the include resolution above. */
static engine *quiet_engine_with(const char *source)
{
    engine *e = engine_new();
    engine_set_logger(e, &silent);
    if (!aussom_parse_string(e, source, "<test>")) return NULL;
    return e;
}

/* An engine with nothing parsed into it yet and its output swallowed. */
static engine *quiet_engine(void)
{
    engine *e = engine_new();
    engine_set_logger(e, &silent);
    return e;
}

/* Writes a property into this engine's security manager, the way a
 * Java test writes into its manager's props. */
static void sm_put(engine *e, const char *name, av_type *v)
{
    av_strmap_put(engine_get_security_manager(e)->props, name, v);
}

static void sm_put_bool(engine *e, const char *name, bool v)
{
    sm_put(e, name, &av_bool_new(v)->base.base);
}

/* --- engine_run, the port of Engine.run -------------------------- */

TEST run_returns_what_main_returns(void)
{
    engine *e = engine_with(
        "class Main { public Main() { } public main() { return 7; } }");
    ASSERT(e != NULL);
    ASSERT_EQ(7, engine_run(e, "<test>"));
    PASS();
}

/* Java's Engine.run returns 0 when main returns something that is not
 * an int; the port has to agree. */
TEST run_returns_zero_when_main_returns_no_int(void)
{
    engine *e = engine_with(
        "class Main { public Main() { } public main() { return \"text\"; } }");
    ASSERT(e != NULL);
    ASSERT_EQ(0, engine_run(e, "<test>"));
    PASS();
}

/* Java throws here. C has no exception to propagate to a host, so the
 * documented behaviour is to log and return 1. */
TEST run_fails_without_a_main_class(void)
{
    engine *e = quiet_engine_with("class NoEntry { public NoEntry() { } }");
    ASSERT(e != NULL);
    ASSERT_EQ(1, engine_run(e, "<test>"));
    PASS();
}

/* The flag is set directly rather than by parsing bad source, because
 * the parser reports syntax errors straight to stderr and that noise
 * would look like a failing test. What is under test here is that
 * engine_run consults the flag at all. */
TEST run_refuses_an_engine_with_parse_errors(void)
{
    engine *e = quiet_engine_with(
        "class Main { public Main() { } public main() { return 7; } }");
    ASSERT(e != NULL);
    engine_set_parse_error(e);
    ASSERT(engine_has_parse_error(e));
    ASSERT_EQ(1, engine_run(e, "<test>"));

    /* And it runs again once the flag is cleared, so the refusal is the
     * flag's doing and not something else about this engine. */
    engine_clear_parse_error(e);
    ASSERT_EQ(7, engine_run(e, "<test>"));
    PASS();
}

/* A program that throws must not take the process with it. */
TEST run_reports_a_runtime_failure(void)
{
    engine *e = quiet_engine_with(
        "class Main { public Main() { } public main() { m = new nosuchclass(); } }");
    ASSERT(e != NULL);
    ASSERT_EQ(1, engine_run(e, "<test>"));
    PASS();
}

/* --- include paths, the port of Engine.addIncludePath ------------ */

/* Java's addIncludePath appends a separator when the caller omits one,
 * because the path is later concatenated with the include name. */
TEST include_path_gains_a_trailing_separator(void)
{
    engine *e = engine_new();
    engine_add_include_path(e, "/tmp/modules");
    engine_add_include_path(e, "/tmp/other/");
    ASSERT_EQ(2, (int)engine_include_path_count(e));
    ASSERT_STR_EQ("/tmp/modules/", engine_get_include_path(e, 0));
    ASSERT_STR_EQ("/tmp/other/", engine_get_include_path(e, 1));
    PASS();
}

TEST exclude_path_gains_a_trailing_separator(void)
{
    engine *e = engine_new();
    engine_add_exclude_path(e, "/etc");
    ASSERT_EQ(1, (int)engine_exclude_path_count(e));
    ASSERT_STR_EQ("/etc/", engine_get_exclude_path(e, 0));
    PASS();
}

/* Java's isPathExcludePath is a prefix test, and it is public there. */
TEST exclude_path_matches_by_prefix(void)
{
    engine *e = engine_new();
    ASSERT_FALSE(engine_is_path_exclude_path(e, "/etc/passwd.aus"));
    engine_add_exclude_path(e, "/etc");
    ASSERT(engine_is_path_exclude_path(e, "/etc/passwd.aus"));
    ASSERT(engine_is_path_exclude_path(e, "/etc/"));
    ASSERT_FALSE(engine_is_path_exclude_path(e, "/etcetera/x.aus"));
    ASSERT_FALSE(engine_is_path_exclude_path(e, "/usr/lib/x.aus"));
    PASS();
}

TEST engines_do_not_share_paths(void)
{
    engine *a = engine_new();
    engine *b = engine_new();
    engine_add_include_path(a, "/only/a");
    ASSERT_EQ(1, (int)engine_include_path_count(a));
    ASSERT_EQ(0, (int)engine_include_path_count(b));
    PASS();
}

/* --- includes resolved from a search path ------------------------ */

TEST include_resolves_from_a_search_path(void)
{
    const char *dir = "build/tests/inc";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", dir);
    ASSERT_EQ(0, system(cmd));

    char libpath[256];
    snprintf(libpath, sizeof libpath, "%s/tlib.aus", dir);
    FILE *f = fopen(libpath, "w");
    ASSERT(f != NULL);
    fputs("class tlib { public tlib() { } public answer() { return 11; } }\n", f);
    fclose(f);

    engine *e = engine_new();
    engine_add_include_path(e, dir);
    ASSERT(aussom_parse_string(e,
        "include tlib;\n"
        "class Main { public Main() { } public main() {"
        "  t = new tlib(); return t.answer(); } }", "<test>"));
    ASSERT_FALSE(engine_has_parse_error(e));
    ASSERT_EQ(11, engine_run(e, "<test>"));
    PASS();
}

/* The same include, refused because its directory is excluded. */
TEST include_from_an_excluded_path_is_refused(void)
{
    const char *dir = "build/tests/inc";
    engine *e = quiet_engine();
    engine_add_include_path(e, dir);
    engine_add_exclude_path(e, dir);
    ASSERT_FALSE(aussom_parse_string(e, "include tlib;\n"
        "class Main { public Main() { } public main() { return 0; } }", "<test>"));
    /* Java's addInclude throws out of the parse: the refusal is the
     * parse exception, not a logged parse error. */
    ASSERT(engine_parse_exception(e) != NULL);
    ASSERT(strstr(engine_parse_exception(e), "from excluded path.") != NULL);
    ASSERT_FALSE(engine_has_parse_error(e));
    ASSERT_EQ(0, (int)engine_parse_diagnostic_count(e));
    PASS();
}

/* An include that is neither a bundled module nor present under any
 * search path is a failure, matching the `if (!found)` throw at the end
 * of Java's Engine.addInclude. Before this was ported the include was
 * discarded without a word, so a program that asked for a module it
 * never got ran to completion and reported success. */
TEST include_not_found_anywhere_is_refused(void)
{
    engine *e = quiet_engine();
    ASSERT_FALSE(aussom_parse_string(e, "include no.such.module;\n"
        "class Main { public Main() { } public main() { return 0; } }", "<test>"));

    /* The error has to name the module: the whole point is that the
     * failure is reported where the fault is rather than showing up
     * later as an undefined name. Java throws it out of the parse. */
    ASSERT_STR_EQ("Engine.addInclude(): Couldn't find requested include "
                  "module 'no/such/module.aus'.", engine_parse_exception(e));
    PASS();
}

/* The refusal is about the module being absent, not about the include
 * statement: the same source resolves cleanly once a search path holds
 * the file. Guards against the not-found report firing on includes that
 * do resolve. */
TEST include_found_after_a_path_is_added(void)
{
    const char *dir = "build/tests/inc";
    engine *bare = quiet_engine();
    ASSERT_FALSE(aussom_parse_string(bare, "include tlib;\n"
        "class Main { public Main() { } public main() { return 0; } }", "<test>"));
    ASSERT(engine_parse_exception(bare) != NULL);
    engine *e = engine_new();
    engine_add_include_path(e, dir);
    ASSERT(aussom_parse_string(e, "include tlib;\n"
        "class Main { public Main() { } public main() {"
        "  t = new tlib(); return t.answer(); } }", "<test>"));
    ASSERT_FALSE(engine_has_parse_error(e));
    ASSERT_EQ(0, (int)engine_parse_diagnostic_count(e));
    ASSERT_EQ(11, engine_run(e, "<test>"));
    PASS();
}

/* --- per-engine logging ------------------------------------------ */

/* Two capturing loggers. The logger vtable carries no context pointer,
 * so each one writes to its own file-scope buffer; that is enough to
 * tell whose output went where. */
static char cap_a[256];
static char cap_b[256];

static void a_capture(const char *s) { snprintf(cap_a, sizeof cap_a, "%s", s); }
static void b_capture(const char *s) { snprintf(cap_b, sizeof cap_b, "%s", s); }
static void a_other(const char *s)   { (void)s; }

/* err and println captured; the rest dropped. */
static const console_logger logger_a = {
    a_other, a_other, a_other, a_other, a_other, a_capture, a_other, a_capture,
};
static const console_logger logger_b = {
    a_other, a_other, a_other, a_other, a_other, b_capture, a_other, b_capture,
};

/* Ports the per-Engine logger from aussom-base: output belongs to the
 * engine that produced it. Before this the logger was process-wide, so
 * whichever engine was configured last captured everyone's output. */
TEST engines_log_to_their_own_logger(void)
{
    engine *a = engine_with(
        "class Main { public Main() { } public main() { c.println(\"from A\"); return 0; } }");
    engine *b = engine_with(
        "class Main { public Main() { } public main() { c.println(\"from B\"); return 0; } }");
    ASSERT(a != NULL && b != NULL);

    engine_set_logger(a, &logger_a);
    engine_set_logger(b, &logger_b);

    cap_a[0] = cap_b[0] = '\0';
    ASSERT_EQ(0, engine_run(a, "<a>"));
    ASSERT_STR_EQ("from A", cap_a);
    ASSERT_STR_EQ("", cap_b);          /* B saw nothing of A's run */

    ASSERT_EQ(0, engine_run(b, "<b>"));
    ASSERT_STR_EQ("from B", cap_b);
    ASSERT_STR_EQ("from A", cap_a);    /* and A's capture is untouched */
    PASS();
}

/* An engine with no logger of its own uses the built-in default, gated
 * by ITS OWN level rather than the process-wide setting, so two engines
 * can share that default at different verbosities. */
TEST engine_log_level_is_per_engine(void)
{
    engine *a = engine_new();
    engine *b = engine_new();
    ASSERT_EQ(CONSOLE_INFO, engine_get_log_level(a));

    engine_set_log_level(a, CONSOLE_TRACE);
    ASSERT_EQ(CONSOLE_TRACE, engine_get_log_level(a));
    ASSERT_EQ(CONSOLE_INFO, engine_get_log_level(b));
    ASSERT_EQ(CONSOLE_INFO, console_get_level());   /* process-wide untouched */
    PASS();
}

/* An installed logger owns its own gating, matching Java where setLevel
 * lives on DefaultLoggingImpl and not on LoggingInt. */
TEST installed_logger_is_not_gated_by_the_engine_level(void)
{
    engine *e = engine_new();
    engine_set_logger(e, &logger_a);
    engine_set_log_level(e, CONSOLE_ERROR);   /* would suppress the default */
    cap_a[0] = '\0';
    engine_println(e, "still written");
    ASSERT_STR_EQ("still written", cap_a);
    PASS();
}

/* --- engine isolation --------------------------------------------- */

/* The property the whole isolation pass exists for: two engines hold
 * DIFFERENT class definition objects for the same base language class.
 * A definition one engine can reach is never one another engine could
 * have altered. Before this, both answered with the Universe's single
 * shared copy. */
TEST engines_hold_distinct_lang_class_defs(void)
{
    engine *a = engine_new();
    engine *b = engine_new();

    ast_class *sa = engine_resolve_class(a, "string");
    ast_class *sb = engine_resolve_class(b, "string");
    ASSERT(sa != NULL);
    ASSERT(sb != NULL);
    ASSERT(sa != sb);

    /* Same for the primitive fast path. */
    ASSERT_EQ(sa, engine_primitive_class_def(a, C_TYPE_STRING));
    ASSERT_EQ(sb, engine_primitive_class_def(b, C_TYPE_STRING));
    ASSERT(engine_primitive_class_def(a, C_TYPE_INT)
           != engine_primitive_class_def(b, C_TYPE_INT));
    PASS();
}

/* Static class instances are per engine too, so one engine cannot
 * observe or alter another's `c`, `math` or `sys`. */
TEST engines_hold_distinct_static_instances(void)
{
    engine *a = engine_new();
    engine *b = engine_new();
    av_type *ca = engine_get_static_class(a, "c");
    av_type *cb = engine_get_static_class(b, "c");
    ASSERT(ca != NULL);
    ASSERT(cb != NULL);
    ASSERT(ca != cb);
    PASS();
}

/* A class one engine defines is invisible to another. This held before
 * the isolation work for user classes; asserted here because with the
 * Universe fallback gone it is now true of every class, base language
 * included. */
TEST classes_do_not_leak_between_engines(void)
{
    engine *a = engine_with(
        "class Mine { public Mine() { } } "
        "class Main { public Main() { } public main() { return 0; } }");
    engine *b = engine_new();
    ASSERT(a != NULL);
    ASSERT(engine_resolve_class(a, "Mine") != NULL);
    ASSERT(engine_resolve_class(b, "Mine") == NULL);
    PASS();
}

/* A primitive carries no class definition -- it does not need one to
 * exist, only to dispatch -- and dispatch resolves it from the engine.
 * Ports the "a primitive built with no engine still dispatches once
 * given one" case from the aussom-base migration notes. */
TEST a_primitive_carries_no_class_def_but_still_dispatches(void)
{
    av_string *s = av_string_new("abc");
    ASSERT(av_object_get_class_def((av_object *)s) == NULL);

    engine *e = engine_with(
        "class Main { public Main() { } public main() {"
        "  if (\"abc\".toUpper() == \"ABC\") { return 7; } return 0; } }");
    ASSERT(e != NULL);
    ASSERT_EQ(7, engine_run(e, "<test>"));
    PASS();
}

/* Syntax errors are engine output too. The parser and lexer write them
 * through the owning engine's logger, so a host capturing one engine's
 * output receives its parse errors and not another engine's. Ports the
 * Scanner.jflex and parser.java half of the same commit. */
TEST parse_errors_go_to_the_engines_logger(void)
{
    engine *a = engine_new();
    engine *b = engine_new();
    engine_set_logger(a, &logger_a);
    engine_set_logger(b, &logger_b);
    cap_a[0] = cap_b[0] = '\0';

    ASSERT_FALSE(aussom_parse_string(a, "class Broken { public (", "bad.aus"));
    ASSERT(engine_has_parse_error(a));
    ASSERT(strstr(cap_a, "PARSE_ERROR") != NULL);
    ASSERT(strstr(cap_a, "bad.aus") != NULL);
    ASSERT_STR_EQ("", cap_b);       /* B heard nothing of A's parse */
    PASS();
}

/* --- concurrent parsing -------------------------------------------- */

/* Each worker builds its own engine and parses source naming classes
 * after its own index, then runs it. With a re-entrant scanner and a
 * pure parser there is no shared parser state, so this is safe; before
 * that it produced bogus syntax errors, flex buffer aborts, GC heap
 * corruption, and classes from one engine's source landing in another.
 *
 * engine_new() itself parses lang.aus, so this exercises concurrent
 * construction as well as concurrent parsing. */
#define PARSE_THREADS 8

static int worker_result[PARSE_THREADS];

static void *parse_worker(void *arg)
{
    long id = (long)arg;
    char src[512];
    snprintf(src, sizeof src,
        "class Helper%ld { public Helper%ld() { } public v() { return %ld; } }\n"
        "class Main { public Main() { } public main() {"
        "  h = new Helper%ld(); return h.v(); } }",
        id, id, id * 7, id);

    engine *e = engine_new();
    engine_set_logger(e, &silent);
    if (!aussom_parse_string(e, src, "<worker>")) { worker_result[id] = -1; return NULL; }
    if (engine_has_parse_error(e)) { worker_result[id] = -2; return NULL; }

    /* Its own class must be present and no other worker's may be. */
    char own[32], other[32];
    snprintf(own, sizeof own, "Helper%ld", id);
    snprintf(other, sizeof other, "Helper%ld", (id + 1) % PARSE_THREADS);
    if (engine_resolve_class(e, own) == NULL)    { worker_result[id] = -3; return NULL; }
    if (engine_resolve_class(e, other) != NULL)  { worker_result[id] = -4; return NULL; }

    worker_result[id] = engine_run(e, "<worker>");
    return NULL;
}

TEST engines_can_parse_concurrently(void)
{
    pthread_t t[PARSE_THREADS];
    for (long i = 0; i < PARSE_THREADS; i++) worker_result[i] = -99;
    for (long i = 0; i < PARSE_THREADS; i++) {
        ASSERT_EQ(0, pthread_create(&t[i], NULL, parse_worker, (void *)i));
    }
    for (int i = 0; i < PARSE_THREADS; i++) pthread_join(t[i], NULL);

    for (long i = 0; i < PARSE_THREADS; i++) {
        /* main() returns i*7, so a wrong value means a worker ran
         * another worker's class. */
        ASSERT_EQ_FMT((int)(i * 7), worker_result[i], "%d");
    }
    PASS();
}

SUITE(engine_run_suite)
{
    RUN_TEST(run_returns_what_main_returns);
    RUN_TEST(run_returns_zero_when_main_returns_no_int);
    RUN_TEST(run_fails_without_a_main_class);
    RUN_TEST(run_refuses_an_engine_with_parse_errors);
    RUN_TEST(run_reports_a_runtime_failure);
    RUN_TEST(engines_log_to_their_own_logger);
    RUN_TEST(engine_log_level_is_per_engine);
    RUN_TEST(installed_logger_is_not_gated_by_the_engine_level);
    RUN_TEST(engines_hold_distinct_lang_class_defs);
    RUN_TEST(engines_hold_distinct_static_instances);
    RUN_TEST(classes_do_not_leak_between_engines);
    RUN_TEST(a_primitive_carries_no_class_def_but_still_dispatches);
    RUN_TEST(parse_errors_go_to_the_engines_logger);
    RUN_TEST(engines_can_parse_concurrently);
}

/* --- per-engine module registry (the port of LangRegistry) -------- */

/* A fresh engine carries the embedded base library. */
TEST a_new_engine_has_the_base_modules(void)
{
    engine *e = engine_new();
    ASSERT(engine_has_module(e, "lang.aus"));
    ASSERT(engine_has_module(e, "math.aus"));
    ASSERT(engine_module_source(e, "math.aus") != NULL);
    ASSERT_FALSE(engine_has_module(e, "nosuch.aus"));
    ASSERT(engine_module_count(e) > 0);
    PASS();
}

/* The point of the registry being an instance: a module given to one
 * engine is not visible to another, so what one engine can include
 * never depends on what another was handed. */
TEST added_modules_do_not_leak_between_engines(void)
{
    engine *a = engine_new();
    engine *b = engine_new();

    engine_add_module(a, "acme.aus",
        "class acme { public acme() { } public answer() { return 41; } }");

    ASSERT(engine_has_module(a, "acme.aus"));
    ASSERT_FALSE(engine_has_module(b, "acme.aus"));
    ASSERT(engine_module_source(b, "acme.aus") == NULL);
    PASS();
}

/* A host-registered module resolves through the ordinary include path,
 * ahead of the filesystem search, exactly as a base module does. */
TEST an_added_module_can_be_included(void)
{
    engine *e = quiet_engine();
    engine_add_module(e, "acme.aus",
        "class acme { public acme() { } public answer() { return 41; } }");
    ASSERT(aussom_parse_string(e,
        "include acme;\n"
        "class Main { public Main() { } public main() {"
        "  a = new acme(); return a.answer(); } }", "<test>"));
    ASSERT_FALSE(engine_has_parse_error(e));
    ASSERT_EQ(41, engine_run(e, "<test>"));
    PASS();
}

/* Registering an existing name replaces it on that engine only, so an
 * engine can be given a different standard library without reaching
 * into anyone else's. Ports LangRegistry.put over Map.put. */
TEST replacing_a_base_module_is_per_engine(void)
{
    engine *a = engine_new();
    engine *b = engine_new();
    const char *original = engine_module_source(b, "math.aus");
    ASSERT(original != NULL);

    engine_add_module(a, "math.aus", "class math { public math() { } }");

    ASSERT_STR_EQ("class math { public math() { } }",
                  engine_module_source(a, "math.aus"));
    ASSERT_EQ(original, engine_module_source(b, "math.aus"));   /* same bytes */
    PASS();
}

/* --- load order: Engine.addInclude and Engine.addClass ----------- */

/* An include is parsed when the parser reaches it, and a static class
 * is instantiated when it is added, so a static constructor can use
 * anything included above it. */
TEST a_static_class_runs_after_the_includes_above_it(void)
{
    engine *e = quiet_engine();
    engine_add_module(e, "acme.aus",
        "class acme { public acme() { } public answer() { return 41; } }");
    ASSERT(aussom_parse_string(e,
        "include acme;\n"
        "static class boot {\n"
        "    public v = 0;\n"
        "    public boot() { a = new acme(); this.v = a.answer(); }\n"
        "    public get() { return this.v; }\n"
        "}\n"
        "class Main { public Main() { } public main() { return boot.get(); } }",
        "<test>"));
    ASSERT_EQ(41, engine_run(e, "<test>"));
    PASS();
}

TEST an_include_is_loaded_once(void)
{
    engine *e = quiet_engine();
    engine_add_module(e, "acme.aus",
        "class acme { public acme() { } public answer() { return 41; } }");
    ASSERT(aussom_parse_string(e,
        "include acme;\ninclude acme;\n"
        "class Main { public Main() { } public main() { return 0; } }", "<test>"));
    int seen = 0;
    for (size_t i = 0; i < engine_include_count(e); i++) {
        if (strcmp(engine_get_include(e, i), "acme.aus") == 0) seen++;
    }
    ASSERT_EQ(1, seen);
    PASS();
}

/* Java throws out of the parse, and so the parse stops with the
 * failure as its exception: "<file> [<line>]: <text>" and the trace. */
TEST a_failing_static_constructor_is_a_parse_error(void)
{
    engine *e = quiet_engine();
    ASSERT_FALSE(aussom_parse_string(e,
        "static class bad { public bad() { nosuch.call(); } }\n"
        "class Main { public Main() { } public main() { return 0; } }", "<test>"));
    ASSERT(engine_parse_exception(e) != NULL);
    ASSERT(strncmp(engine_parse_exception(e), "<test> [1]: ", 12) == 0);
    /* The class after it was never read. */
    ASSERT(engine_get_class_by_name(e, "Main") == NULL);
    PASS();
}

/* Ports astExpression.evalInclude: a refused include at run time is an
 * INCLUDE_FAILED exception, and does not mark the engine as failed. */
TEST a_runtime_include_failure_throws_include_failed(void)
{
    engine *e = quiet_engine();
    ASSERT(aussom_parse_string(e,
        "class Main { public Main() { } public main() {\n"
        "    try { include no.such.module; }\n"
        "    catch (ex) { if (ex.getId() == \"INCLUDE_FAILED\") { return 5; } }\n"
        "    return 0;\n"
        "} }", "<test>"));
    ASSERT_EQ(5, engine_run(e, "<test>"));
    ASSERT_FALSE(engine_has_parse_error(e));
    PASS();
}

/* --- reflect.includeModule (AReflect.includeModule) -------------- */

/* Writes build/tests/rmod/net.aus, the file module name
 * "build.tests.rmod.net" names relative to the working directory. */
static int write_rmod(void)
{
    if (system("mkdir -p build/tests/rmod") != 0) return 0;
    FILE *f = fopen("build/tests/rmod/net.aus", "w");
    if (f == NULL) return 0;
    fputs("class rnet { public rnet() { } public answer() { return 17; } }\n", f);
    fclose(f);
    return 1;
}

/* The dotted name is a path, parsed with parseFile, and is not an
 * include: the include list does not grow. */
TEST include_module_parses_the_file_its_name_names(void)
{
    ASSERT(write_rmod());
    engine *e = quiet_engine();
    sm_put_bool(e, "reflect.include.module", true);
    ASSERT(aussom_parse_string(e,
        "include reflect;\n"
        "class Main { public Main() { } public main() {\n"
        "    reflect.includeModule(\"build.tests.rmod.net\");\n"
        "    r = new rnet(); return r.answer();\n"
        "} }", "<test>"));
    size_t includes = engine_include_count(e);
    ASSERT_EQ(17, engine_run(e, "<test>"));
    ASSERT_EQ(includes, engine_include_count(e));
    PASS();
}

/* A file that cannot be read is an exception, as Java's parseFile
 * throws, and does not mark the engine as failed. */
TEST include_module_of_a_missing_file_throws(void)
{
    engine *e = quiet_engine();
    sm_put_bool(e, "reflect.include.module", true);
    ASSERT(aussom_parse_string(e,
        "include reflect;\n"
        "class Main { public Main() { } public main() {\n"
        "    try { reflect.includeModule(\"no.such.mod\"); }\n"
        "    catch (ex) { if (ex.getText().contains(\"no/such/mod.aus\")) { return 5; } }\n"
        "    return 0;\n"
        "} }", "<test>"));
    ASSERT_EQ(5, engine_run(e, "<test>"));
    ASSERT_FALSE(engine_has_parse_error(e));
    PASS();
}

TEST include_module_is_refused_by_default(void)
{
    ASSERT(write_rmod());
    engine *e = quiet_engine();
    ASSERT(aussom_parse_string(e,
        "include reflect;\n"
        "class Main { public Main() { } public main() {\n"
        "    try { reflect.includeModule(\"build.tests.rmod.net\"); }\n"
        "    catch (ex) { if (ex.getText().contains(\"not permitted\")) { return 5; } }\n"
        "    return 0;\n"
        "} }", "<test>"));
    ASSERT_EQ(5, engine_run(e, "<test>"));
    PASS();
}

/* Ports AReflect.evalStr's return value: the calling instance. */
TEST eval_str_returns_the_calling_instance(void)
{
    engine *e = quiet_engine();
    sm_put_bool(e, "reflect.eval.string", true);
    ASSERT(aussom_parse_string(e,
        "include reflect;\n"
        "class Main { public Main() { } public main() {\n"
        "    r = reflect.evalStr(\"class evq { public evq() { } }\");\n"
        "    if (r == null) { return 0; }\n"
        "    return 1;\n"
        "} }", "<test>"));
    ASSERT_EQ(1, engine_run(e, "<test>"));
    PASS();
}

/* --- parse errors aussom-base throws ------------------------------ */

/* A module whose own parse throws (a duplicate overload). */
static const char BAD_MODULE[] =
    "class badmod { public f(int a) { } public f(int b) { } }\n";

/* The included source's parse stops, and the error passes up through
 * the include and stops the parse that asked for it, as Java's
 * exception does. Nothing after the include is read. */
TEST an_include_whose_source_throws_stops_the_parse(void)
{
    engine *e = quiet_engine();
    engine_add_module(e, "badmod.aus", BAD_MODULE);
    ASSERT_FALSE(aussom_parse_string(e, "include badmod;\n"
        "class Main { public Main() { } public main() { return 0; } }", "<test>"));
    /* A module is parsed under Java's resource path. */
    ASSERT_STR_EQ(" [0]: Duplicate method overload 'f(i)' declared at "
                  "/com/aussom/stdlib/aus/badmod.aus:1; first declared at "
                  "/com/aussom/stdlib/aus/badmod.aus:1.\n",
                  engine_parse_exception(e));
    ASSERT_STR_EQ("Duplicate method overload 'f(i)' declared at "
                  "/com/aussom/stdlib/aus/badmod.aus:1; first declared at "
                  "/com/aussom/stdlib/aus/badmod.aus:1.",
                  engine_parse_exception_stack_trace(e));
    ASSERT(engine_get_class_by_name(e, "Main") == NULL);
    ASSERT_FALSE(engine_has_parse_error(e));
    PASS();
}

/* astExpression.evalInclude: the same error at run time is
 * INCLUDE_FAILED. */
TEST a_runtime_include_whose_source_throws_fails(void)
{
    engine *e = quiet_engine();
    engine_add_module(e, "badmod.aus", BAD_MODULE);
    ASSERT(aussom_parse_string(e,
        "class Main { public Main() { } public main() {\n"
        "    try { include badmod; }\n"
        "    catch (ex) { if (ex.getId() == \"INCLUDE_FAILED\"\n"
        "        && ex.getText().contains(\"Duplicate method overload\")) { return 5; } }\n"
        "    return 0;\n"
        "} }", "<test>"));
    ASSERT_EQ(5, engine_run(e, "<test>"));
    PASS();
}

/* parser.report_error: one diagnostic with Java's text, and the parse
 * stops there, so the second error is never seen. */
TEST a_syntax_error_stops_at_the_first(void)
{
    engine *e = quiet_engine();
    ASSERT_FALSE(aussom_parse_string(e,
        "class A { public f( { } }\nclass B { public g( { } }", "<test>"));
    ASSERT(engine_has_parse_error(e));
    ASSERT(engine_parse_exception(e) == NULL);
    ASSERT_EQ(1, (int)engine_parse_diagnostic_count(e));
    const parse_diagnostic *d = engine_get_parse_diagnostic(e, 0);
    ASSERT_EQ(1, d->line);
    ASSERT_EQ(21, d->col);
    ASSERT_STR_EQ("PARSE_ERROR: Unknown symbol found at line 1 column 21.", d->message);
    PASS();
}

/* Engine.parseFile: a file that can't be read throws
 * FileNotFoundException, "<path> (<reason>)". */
TEST parsing_a_missing_file_throws(void)
{
    engine *e = quiet_engine();
    ASSERT_FALSE(aussom_parse_file(e, "build/tests/no_such_file.aus"));
    ASSERT_STR_EQ("build/tests/no_such_file.aus (No such file or directory)",
                  engine_parse_exception(e));
    ASSERT_FALSE(engine_has_parse_error(e));
    PASS();
}

/* AReflect.evalStr returns what the parse threw, with its message. */
TEST eval_str_returns_a_thrown_parse_error(void)
{
    engine *e = quiet_engine();
    sm_put_bool(e, "reflect.eval.string", true);
    ASSERT(aussom_parse_string(e,
        "include reflect;\n"
        "class Main { public Main() { } public main() {\n"
        "    try { reflect.evalStr(\"class evb { public f(int a) { } public f(int b) { } }\", \"evb\"); }\n"
        "    catch (ex) { if (ex.getText() == \" [0]: Duplicate method overload 'f(i)' declared at evb:1; first declared at evb:1.\\n\") { return 5; } }\n"
        "    return 0;\n"
        "} }", "<test>"));
    ASSERT_EQ(5, engine_run(e, "<test>"));
    PASS();
}

/* Engine.parseStatements: in script mode a thrown error becomes an
 * ordinary parse error, logged and recorded at file level. */
TEST a_thrown_error_in_script_mode_is_a_parse_error(void)
{
    engine *e = quiet_engine();
    sm_put_bool(e, "aussom.script.mode.enable", true);
    ast_statement_list *script = ast_statement_list_new();
    ASSERT_FALSE(aussom_parse_string_script(e, "a = 99999999999999999999;",
                                            "<eval>", script));
    ASSERT(engine_has_parse_error(e));
    ASSERT_EQ(1, (int)engine_parse_diagnostic_count(e));
    const parse_diagnostic *d = engine_get_parse_diagnostic(e, 0);
    ASSERT_EQ(PARSE_DIAG_NO_POSITION, d->line);
    ASSERT_STR_EQ("<eval> [1]: PARSE_ERROR: Integer out of bounds. Integer max "
                  "size is 9223372036854775807. <99999999999999999999>\n",
                  d->message);
    PASS();
}

SUITE(load_order)
{
    RUN_TEST(a_static_class_runs_after_the_includes_above_it);
    RUN_TEST(an_include_is_loaded_once);
    RUN_TEST(a_failing_static_constructor_is_a_parse_error);
    RUN_TEST(a_runtime_include_failure_throws_include_failed);
    RUN_TEST(include_module_parses_the_file_its_name_names);
    RUN_TEST(include_module_of_a_missing_file_throws);
    RUN_TEST(include_module_is_refused_by_default);
    RUN_TEST(eval_str_returns_the_calling_instance);
    RUN_TEST(an_include_whose_source_throws_stops_the_parse);
    RUN_TEST(a_runtime_include_whose_source_throws_fails);
    RUN_TEST(a_syntax_error_stops_at_the_first);
    RUN_TEST(parsing_a_missing_file_throws);
    RUN_TEST(eval_str_returns_a_thrown_parse_error);
    RUN_TEST(a_thrown_error_in_script_mode_is_a_parse_error);
}

SUITE(include_paths)
{
    RUN_TEST(include_path_gains_a_trailing_separator);
    RUN_TEST(exclude_path_gains_a_trailing_separator);
    RUN_TEST(exclude_path_matches_by_prefix);
    RUN_TEST(engines_do_not_share_paths);
    RUN_TEST(include_resolves_from_a_search_path);
    RUN_TEST(include_from_an_excluded_path_is_refused);
    RUN_TEST(include_not_found_anywhere_is_refused);
    RUN_TEST(include_found_after_a_path_is_added);
    RUN_TEST(a_new_engine_has_the_base_modules);
    RUN_TEST(added_modules_do_not_leak_between_engines);
    RUN_TEST(an_added_module_can_be_included);
    RUN_TEST(replacing_a_base_module_is_per_engine);
}

/* --- native stack guard (see include/aussom/stack_guard.h) -------- */

/* The interpreter recurses on the native stack, and in C running out is
 * SIGSEGV for the whole process. The stack guard stops first and raises
 * STACK_OVERFLOW, the id Java's engine reports for a StackOverflowError.
 * These run real overflows, on the main thread and on threads with
 * small stacks like the ones a mobile host gives its interpreter
 * threads, and check that each ends in the exception rather than a
 * crash. tests/c-interpreter.aus covers the value walkers (toJson, pack,
 * clone, printing) on whatever stack it runs on. */

/* Every program below ends main by returning one of these. */
#define SO_CAUGHT 7     /* caught STACK_OVERFLOW */
#define SO_DEPTH  8     /* caught CALL_DEPTH_EXCEEDED */
#define SO_OTHER  9     /* caught something else */
#define SO_NONE   10    /* nothing was thrown */

/* Wraps `body` in a main that reports what it caught. */
static char *so_program(const char *members, const char *body)
{
    const char *fmt =
        "class Main {\n"
        "  public Main() { }\n"
        "%s\n"
        "  public main() {\n"
        "    try { %s } catch (e) {\n"
        "      if (e.getId() == \"STACK_OVERFLOW\") { return 7; }\n"
        "      if (e.getId() == \"CALL_DEPTH_EXCEEDED\") { return 8; }\n"
        "      return 9;\n"
        "    }\n"
        "    return 10;\n"
        "  }\n"
        "}\n";
    size_t n = strlen(fmt) + strlen(members) + strlen(body) + 1;
    char *src = malloc(n);
    snprintf(src, n, fmt, members, body);
    return src;
}

/* `x = 1 + 1 + ... + 1;` with `terms` terms. Left-associative, so bison
 * parses it in constant parser stack but the tree is `terms` deep. */
static char *so_plus_chain(int terms)
{
    size_t n = (size_t)terms * 4 + 16;
    char *s = malloc(n);
    char *p = s;
    p += sprintf(p, "x = 1");
    for (int i = 1; i < terms; i++) p += sprintf(p, " + 1");
    sprintf(p, ";");
    return s;
}

/* `x = !!...!true;` with `count` operators. Right-nested; bison's own
 * stack limit (10,000) bounds how many it accepts. */
static char *so_not_chain(int count)
{
    char *s = malloc((size_t)count + 32);
    char *p = s;
    p += sprintf(p, "x = ");
    for (int i = 0; i < count; i++) *p++ = '!';
    sprintf(p, "true;");
    return s;
}

static engine *so_engine(const char *members, const char *body)
{
    char *src = so_program(members, body);
    engine *e = quiet_engine();
    bool ok = aussom_parse_string(e, src, "<stack>");
    free(src);
    return ok ? e : NULL;
}

/* engine_run on a thread with its own stack size, so a test does not
 * depend on the main thread's stack (8 MB on Linux and macOS, 1 MB on
 * Windows). */
typedef struct { engine *e; int result; } so_job;

static void *so_run(void *arg)
{
    so_job *job = arg;
    job->result = engine_run(job->e, "<stack>");
    return NULL;
}

static int so_run_on_stack(engine *e, size_t stack_bytes)
{
    so_job job = { e, -1 };
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    if (pthread_create(&t, &attr, so_run, &job) != 0) {
        pthread_attr_destroy(&attr);
        return -2;
    }
    pthread_attr_destroy(&attr);
    pthread_join(t, NULL);
    return job.result;
}

#define KB(n) ((size_t)(n) * 1024)

/* A chain of 200,000 terms needs about 80 MB of stack, more than any
 * main thread has. */
TEST a_deep_expression_raises_stack_overflow(void)
{
    char *body = so_plus_chain(200000);
    engine *e = so_engine("", body);
    free(body);
    ASSERT(e != NULL);
    ASSERT_EQ(SO_CAUGHT, engine_run(e, "<stack>"));
    PASS();
}

TEST a_deep_expression_raises_stack_overflow_on_small_stacks(void)
{
    static const size_t sizes[] = { KB(512), KB(1024), KB(2048) };
    char *body = so_plus_chain(200000);
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        engine *e = so_engine("", body);
        ASSERT(e != NULL);
        ASSERT_EQ_FMT(SO_CAUGHT, so_run_on_stack(e, sizes[i]), "%d");
    }
    free(body);
    PASS();
}

/* A unary chain uses about 1 KB per level, so 9,000 (inside bison's
 * limit) overflows a 1 MB thread. */
TEST a_deep_unary_chain_raises_stack_overflow(void)
{
    char *body = so_not_chain(9000);
    engine *e = so_engine("", body);
    free(body);
    ASSERT(e != NULL);
    ASSERT_EQ(SO_CAUGHT, so_run_on_stack(e, KB(1024)));
    PASS();
}

/* An expression that fits runs normally, so the guard is not firing
 * early. */
TEST a_modest_expression_is_not_an_overflow(void)
{
    char *body = so_plus_chain(500);
    engine *e = so_engine("", body);
    free(body);
    ASSERT(e != NULL);
    ASSERT_EQ(SO_NONE, so_run_on_stack(e, KB(1024)));
    PASS();
}

static const char *so_recurse_members =
    "  public r(n) { return this.r(n + 1) + 1; }\n";

/* With the call depth limit off, nothing but the guard stands between
 * runaway recursion and SIGSEGV. */
TEST runaway_recursion_without_a_call_limit_raises_stack_overflow(void)
{
    static const size_t sizes[] = { KB(512), KB(1024), KB(8192) };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        engine *e = so_engine(so_recurse_members, "this.r(0);");
        ASSERT(e != NULL);
        sm_put(e, ENGINE_LIMIT_CALL_DEPTH_PROP, &av_int_new(0)->base.base);
        ASSERT_EQ_FMT(SO_CAUGHT, so_run_on_stack(e, sizes[i]), "%d");
    }
    PASS();
}

/* On a stack large enough for it, the call depth limit still fires
 * first and keeps its own id. 1,000 calls need about 1.35 MB. */
TEST the_call_depth_limit_fires_first_on_a_large_stack(void)
{
    engine *e = so_engine(so_recurse_members, "this.r(0);");
    ASSERT(e != NULL);
    ASSERT_EQ(SO_DEPTH, so_run_on_stack(e, KB(16384)));
    PASS();
}

/* With the default limit on a stack too small for 1,000 calls, the
 * guard fires before the limit: an exception, where this used to be
 * SIGSEGV. */
TEST recursion_on_a_small_stack_raises_stack_overflow(void)
{
    engine *e = so_engine(so_recurse_members, "this.r(0);");
    ASSERT(e != NULL);
    ASSERT_EQ(SO_CAUGHT, so_run_on_stack(e, KB(512)));
    PASS();
}

/* Printing deeply nested data that does not contain itself. Its printed
 * form grows with the square of the depth, so this runs on small
 * stacks, where the guard stops it quickly; tests/c-interpreter.aus
 * leaves it out for that reason. */
TEST printing_deep_data_raises_stack_overflow_on_small_stacks(void)
{
    static const size_t sizes[] = { KB(512), KB(1024) };
    static const char *bodies[] = {
        "l = []; for (i = 0; i < 200000; i++) { l = [l]; } s = '' + l;",
        "l = []; for (i = 0; i < 200000; i++) { l = [l]; } c.log(l);",
        "m = {}; for (i = 0; i < 200000; i++) { m = {'m': m}; } s = '' + m;",
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (size_t j = 0; j < sizeof bodies / sizeof bodies[0]; j++) {
            engine *e = so_engine("", bodies[j]);
            ASSERT(e != NULL);
            ASSERT_EQ_FMT(SO_CAUGHT, so_run_on_stack(e, sizes[i]), "%d");
        }
    }
    PASS();
}

/* The reserve measurement: the deepest C work a script can reach runs
 * at every level of a runaway recursion, so on some level it runs just
 * above the point where the guard fires. A reserve too small for this
 * platform's frames crashes the test binary here. */
TEST heavy_c_work_near_the_limit_does_not_crash(void)
{
    static const char *members =
        "  public dj = '';\n"
        "  public cmp(x, y) { return x - y; }\n"
        "  public lvl(d) {\n"
        "    a = json.parse(this.dj);\n"
        "    m = regex.match('(a|b)*c+(d?)e', 'ababababcccde');\n"
        "    s = 'x' + 3.14159 + ' ' + d + ' ' + [1, 2.5, {'k': [true, null]}];\n"
        "    l = [5, 3, 9, 1]; l.sortCustom(::cmp);\n"
        "    j = [l, {'x': s}].toJson(); p = [l, {'x': s}].pack();\n"
        "    return this.lvl(d + 1) + 1;\n"
        "  }\n";
    static const char *body =
        "o = ''; cl = ''; for (i = 0; i < 999; i++) { o += '['; cl += ']'; }"
        " this.dj = o + '1' + cl; this.lvl(0);";
    static const size_t sizes[] = { KB(512), KB(768), KB(1024), KB(2048) };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char *src = so_program(members, body);
        size_t n = strlen(src) + 32;
        char *full = malloc(n);
        snprintf(full, n, "include util;\n%s", src);
        free(src);
        engine *e = quiet_engine();
        bool ok = aussom_parse_string(e, full, "<stack>");
        free(full);
        ASSERT(ok);
        sm_put(e, ENGINE_LIMIT_CALL_DEPTH_PROP, &av_int_new(0)->base.base);
        ASSERT_EQ_FMT(SO_CAUGHT, so_run_on_stack(e, sizes[i]), "%d");
    }
    PASS();
}

/* Uncaught, the overflow ends the program as a failure, and the program
 * keeps working after a caught one. */
TEST an_uncaught_overflow_fails_the_run_and_a_caught_one_does_not(void)
{
    char *chain = so_plus_chain(200000);
    size_t n = strlen(chain) + 256;
    char *src = malloc(n);
    snprintf(src, n,
        "class Main { public Main() { } public main() { %s return 0; } }", chain);
    engine *e = quiet_engine();
    ASSERT(aussom_parse_string(e, src, "<stack>"));
    ASSERT_EQ(1, engine_run(e, "<stack>"));

    snprintf(src, n,
        "class Main { public Main() { } public main() {"
        " try { %s } catch (e) { }"
        " l = [1, [2, 3]]; if (l.toJson() == '[1,[2,3]]') { return 5; } return 6; } }",
        chain);
    e = quiet_engine();
    ASSERT(aussom_parse_string(e, src, "<stack>"));
    ASSERT_EQ(5, engine_run(e, "<stack>"));
    free(src);
    free(chain);
    PASS();
}

SUITE(stack_guard)
{
    RUN_TEST(a_deep_expression_raises_stack_overflow);
    RUN_TEST(a_deep_expression_raises_stack_overflow_on_small_stacks);
    RUN_TEST(a_deep_unary_chain_raises_stack_overflow);
    RUN_TEST(a_modest_expression_is_not_an_overflow);
    RUN_TEST(runaway_recursion_without_a_call_limit_raises_stack_overflow);
    RUN_TEST(the_call_depth_limit_fires_first_on_a_large_stack);
    RUN_TEST(recursion_on_a_small_stack_raises_stack_overflow);
    RUN_TEST(printing_deep_data_raises_stack_overflow_on_small_stacks);
    RUN_TEST(heavy_c_work_near_the_limit_does_not_crash);
    RUN_TEST(an_uncaught_overflow_fails_the_run_and_a_caught_one_does_not);
}

/* --- security gates (report L3) ----------------------------------
 *
 * Each gate is run twice on a fresh engine: once under the default
 * policy and once with the property changed, so both the deny path and
 * the allow path are pinned. An Aussom test file cannot do this for
 * the test.* gates, because the test runner turns those on. */

/* Runs `body` as main's body on a fresh, quiet engine, after setting
 * `prop` to `allow` (when prop is not NULL). `head` goes before the
 * class (includes, helper classes). Returns main's return value, or -1
 * when the source does not parse. */
static int gate_run(const char *head, const char *prop, bool allow, const char *body)
{
    engine *e = quiet_engine();
    if (prop != NULL) sm_put_bool(e, prop, allow);
    size_t n = strlen(head) + strlen(body) + 128;
    char *src = malloc(n);
    snprintf(src, n, "%s\nclass Main { public Main() { } public main() {\n%s\n} }", head, body);
    bool ok = aussom_parse_string(e, src, "<gate>");
    free(src);
    if (!ok) return -1;
    return engine_run(e, "<gate>");
}

/* Main returns 1 when the call threw a "not permitted" exception, 2
 * when it returned without throwing. */
#define GATE_CALL(stmt)                                                     \
    "try { " stmt " } catch (ex) {\n"                                       \
    "  if (ex.getText().contains(\"not permitted\")) { return 1; }\n"      \
    "  return 3;\n"                                                        \
    "}\nreturn 2;"
#define GATE_DENIED 1
#define GATE_ALLOWED 2

static void gate_setenv(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* Each sys getter returns null when its property is off and a string
 * when it is on. The variables are set so the allowed value always
 * exists. java.info.view and java.classpath.view have no getter that
 * reads them in C: with no JVM, those getters always return null. */
TEST sys_view_gates_deny_by_default_and_allow_when_set(void)
{
    static const struct { const char *prop, *call; } views[] = {
        { "os.info.view",      "sys.getOsName()" },
        { "os.info.view",      "sys.getFileSeparator()" },
        { "java.home.view",    "sys.getJavaHome()" },
        { "aussom.info.view",  "sys.getAussomVersion()" },
        { "aussom.info.view",  "sys.getAssembly()" },
        { "aussom.path.view",  "sys.getAssemblyPath()" },
        { "current.path.view", "sys.getCurrentPath()" },
        { "home.path.view",    "sys.getHomePath()" },
        { "user.name.view",    "sys.getUserName()" },
    };
    gate_setenv("JAVA_HOME", "/gate/java");
    gate_setenv("HOME", "/gate/home");
    gate_setenv("USER", "gateuser");
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        char body[256];
        snprintf(body, sizeof body,
            "v = %s; if (v == null) { return 1; } if (v instanceof 'string') { return 2; } return 3;",
            views[i].call);
        ASSERT_EQ_FMTm(views[i].call, 1, gate_run("include sys;", NULL, false, body), "%d");
        ASSERT_EQ_FMTm(views[i].call, 2, gate_run("include sys;", views[i].prop, true, body), "%d");
    }
    PASS();
}

TEST reflect_eval_string_gate(void)
{
    const char *body = GATE_CALL("reflect.evalStr(\"class gq { public gq() { } }\");");
    ASSERT_EQ(GATE_DENIED, gate_run("include reflect;", NULL, false, body));
    ASSERT_EQ(GATE_ALLOWED, gate_run("include reflect;", "reflect.eval.string", true, body));
    PASS();
}

TEST reflect_eval_file_gate(void)
{
    ASSERT_EQ(0, system("mkdir -p build/tests/gate"));
    FILE *f = fopen("build/tests/gate/evf.aus", "w");
    ASSERT(f != NULL);
    fputs("class gevf { public gevf() { } public answer() { return 9; } }\n", f);
    fclose(f);
    const char *body = GATE_CALL("reflect.evalFile(\"build/tests/gate/evf.aus\");");
    ASSERT_EQ(GATE_DENIED, gate_run("include reflect;", NULL, false, body));
    ASSERT_EQ(GATE_ALLOWED, gate_run("include reflect;", "reflect.eval.file", true, body));
    /* Allowed means the file was really loaded. */
    ASSERT_EQ(9, gate_run("include reflect;", "reflect.eval.file", true,
        "reflect.evalFile(\"build/tests/gate/evf.aus\"); g = new gevf(); return g.answer();"));
    PASS();
}

TEST reflect_clear_diagnostics_gate(void)
{
    const char *body = GATE_CALL("reflect.clearParseDiagnostics();");
    ASSERT_EQ(GATE_DENIED, gate_run("include reflect;", NULL, false, body));
    ASSERT_EQ(GATE_ALLOWED, gate_run("include reflect;", "reflect.clear.diagnostics", true, body));
    PASS();
}

/* Off in SecurityManagerImpl; DefaultSecurityManagerImpl turns it on.
 * Without it ALang.getClassAussomdoc returns null rather than throwing. */
TEST aussomdoc_class_get_json_gate(void)
{
    const char *body =
        "d = lang.getClassAussomdoc(\"Main\");\n"
        "if (d == null) { return 1; }\n"
        "return 2;";
    ASSERT_EQ(1, gate_run("", NULL, false, body));
    ASSERT_EQ(2, gate_run("", "aussomdoc.class.getJson", true, body));
    PASS();
}

TEST securitymanager_gates(void)
{
    const char *make = GATE_CALL("s = new SecurityManager();");
    ASSERT_EQ(GATE_ALLOWED, gate_run("", NULL, false, make));
    ASSERT_EQ(GATE_DENIED, gate_run("", "securitymanager.instantiate", false, make));
    /* A child of a refused parent is refused too, not built half-made. */
    const char *child = GATE_CALL("k = new gchild();");
    ASSERT_EQ(GATE_ALLOWED, gate_run("class gchild : SecurityManager { }", NULL, false, child));
    ASSERT_EQ(GATE_DENIED, gate_run("class gchild : SecurityManager { }",
                                    "securitymanager.instantiate", false, child));

    const char *get = GATE_CALL("secman.getProp(\"os.info.view\");");
    ASSERT_EQ(GATE_ALLOWED, gate_run("", NULL, false, get));
    ASSERT_EQ(GATE_DENIED, gate_run("", "securitymanager.property.get", false, get));

    const char *list = GATE_CALL("secman.keySet();");
    ASSERT_EQ(GATE_ALLOWED, gate_run("", NULL, false, list));
    ASSERT_EQ(GATE_DENIED, gate_run("", "securitymanager.property.list", false, list));
    const char *map = GATE_CALL("secman.getMap();");
    ASSERT_EQ(GATE_DENIED, gate_run("", "securitymanager.property.list", false, map));

    const char *set = GATE_CALL("secman.setProp(\"os.info.view\", true);");
    ASSERT_EQ(GATE_DENIED, gate_run("", NULL, false, set));
    ASSERT_EQ(GATE_ALLOWED, gate_run("", "securitymanager.property.set", true, set));
    PASS();
}

TEST test_runner_gate(void)
{
    const char *body = GATE_CALL("t = new testRunner();");
    ASSERT_EQ(GATE_DENIED, gate_run("include aunit;", NULL, false, body));
    ASSERT_EQ(GATE_ALLOWED, gate_run("include aunit;", "test.aussom.runner", true, body));
    PASS();
}

static const char *const gate_mock_class =
    "class gm { public gm() { } public f() { return 1; } }";

TEST test_mock_inject_gate(void)
{
    const char *body = GATE_CALL("g = new gm(); g.mock(\"f\", 5); if (g.f() != 5) { return 4; }");
    ASSERT_EQ(GATE_DENIED, gate_run(gate_mock_class, NULL, false, body));
    ASSERT_EQ(GATE_ALLOWED, gate_run(gate_mock_class, "test.mock.inject", true, body));
    PASS();
}

TEST test_mock_spy_gate(void)
{
    const char *body = GATE_CALL(
        "g = new gm(); g.setSpy(\"f\"); g.f(); if (#g.getSpy(\"f\") != 1) { return 4; }");
    ASSERT_EQ(GATE_DENIED, gate_run(gate_mock_class, NULL, false, body));
    ASSERT_EQ(GATE_ALLOWED, gate_run(gate_mock_class, "test.mock.spy", true, body));
    PASS();
}

/* Parses an extern declaration with the allowlist on (when `allowed`
 * is not NULL) and the given single entry. True when it parses. */
static bool gate_extern_parses(const char *allowed)
{
    engine *e = quiet_engine();
    if (allowed != NULL) {
        sm_put_bool(e, "aussom.extern.allowlist.enforce", true);
        av_list *l = av_list_new();
        av_list_add(l, &av_string_new(allowed)->base.base);
        sm_put(e, "aussom.extern.allowed", &l->base.base);
    }
    return aussom_parse_string(e,
        "extern class gu : com.aussom.stdlib.AUuid { public extern get(); }\n"
        "class Main { public Main() { } public main() { return 0; } }", "<gate>");
}

TEST extern_allowlist_gate(void)
{
    ASSERT(gate_extern_parses(NULL));
    ASSERT(gate_extern_parses("com.aussom.stdlib.AUuid"));
    ASSERT(gate_extern_parses("com.aussom.stdlib.*"));
    ASSERT_FALSE(gate_extern_parses("com.aussom.stdlib.ADate"));
    ASSERT_FALSE(gate_extern_parses("com.aussom.std.*"));
    PASS();
}

#if !defined(_WIN32)
#include <unistd.h>

/* On by default; off refuses an include reached through a link. */
TEST include_symlink_follow_gate(void)
{
    ASSERT_EQ(0, system("rm -rf build/tests/gatelnk && mkdir -p build/tests/gatelnk/real"));
    FILE *f = fopen("build/tests/gatelnk/real/glm.aus", "w");
    ASSERT(f != NULL);
    fputs("class glm { public glm() { } public answer() { return 13; } }\n", f);
    fclose(f);
    ASSERT_EQ(0, symlink("real", "build/tests/gatelnk/lnk"));
    const char *src = "include lnk.glm;\n"
        "class Main { public Main() { } public main() { g = new glm(); return g.answer(); } }";

    engine *e = quiet_engine();
    engine_add_include_path(e, "build/tests/gatelnk");
    ASSERT(aussom_parse_string(e, src, "<gate>"));
    ASSERT_EQ(13, engine_run(e, "<gate>"));

    e = quiet_engine();
    sm_put_bool(e, "aussom.include.symlink.follow", false);
    engine_add_include_path(e, "build/tests/gatelnk");
    ASSERT_FALSE(aussom_parse_string(e, src, "<gate>"));
    PASS();
}
#endif

/* Script mode (report L2): Java checks aussom.script.mode.enable in
 * parseScriptLine and evalParsedScript, which are these two. */
TEST script_mode_gate(void)
{
    engine *e = quiet_engine();
    ast_statement_list *script = ast_statement_list_new();
    ASSERT_FALSE(aussom_parse_string_script(e, "a = 2 + 3;", "<gate>", script));
    av_type *r = eval_run_statements(e, script, av_strmap_new_unlocked());
    ASSERT(av_is_ex(r));
    ASSERT(strstr(av_str(r), "aussom.script.mode.enable") != NULL);

    e = quiet_engine();
    sm_put_bool(e, "aussom.script.mode.enable", true);
    script = ast_statement_list_new();
    ASSERT(aussom_parse_string_script(e, "a = 2 + 3;", "<gate>", script));
    av_strmap *locals = av_strmap_new_unlocked();
    r = eval_run_statements(e, script, locals);
    ASSERT_FALSE(av_is_ex(r));
    av_type *a = av_strmap_get(locals, "a");
    ASSERT(a != NULL && a->type == C_TYPE_INT);
    ASSERT_EQ(5, (int)av_int_get((av_int *)a));
    PASS();
}

/* An unknown string escape (report L7) is a lexer error, as in Java's
 * scanner, not text that flex's default rule copies to stdout. */
TEST an_unknown_string_escape_is_a_lex_error(void)
{
    static const char *const sources[] = {
        "class Main { public Main() { } public main() { s = \"a\\qb\"; return 0; } }",
        "class Main { public Main() { } public main() { s = 'a\\qb'; return 0; } }",
    };
    for (size_t i = 0; i < 2; i++) {
        engine *e = quiet_engine();
        ASSERT_FALSE(aussom_parse_string(e, sources[i], "<gate>"));
        bool found = false;
        for (size_t j = 0; j < engine_parse_diagnostic_count(e); j++) {
            if (strstr(engine_get_parse_diagnostic(e, j)->message,
                       "Illegal character <92>") != NULL) found = true;
        }
        ASSERT(found);
    }
    PASS();
}

/* An illegal character is reported as Java's lexer reports it: its
 * UTF-16 unit value, at the column it starts, with columns counted in
 * UTF-16 units. A character past U+FFFF is two errors. */
TEST an_illegal_character_reports_java_columns(void)
{
    static const char *const src =
        "class Main { public main() { s = \"\xc3\xa9\xc3\xa9\"; \\ t = 1; \xc3\xa9 "
        "u = \xf0\x9f\x98\x80; return 0; } }";
    static const struct { int col; const char *msg; } want[] = {
        { 40, "Illegal character <92>" },
        { 49, "Illegal character <233>" },
        { 55, "Illegal character <55357>" },
        { 56, "Illegal character <56832>" },
    };
    engine *e = quiet_engine();
    ASSERT_FALSE(aussom_parse_string(e, src, "<gate>"));
    ASSERT(engine_parse_diagnostic_count(e) >= 4);
    for (size_t i = 0; i < 4; i++) {
        const parse_diagnostic *d = engine_get_parse_diagnostic(e, i);
        ASSERT_EQ(1, d->line);
        ASSERT_EQ(want[i].col, d->col);
        ASSERT_STR_EQ(want[i].msg, d->message);
    }
    PASS();
}

/* engine_cancel must reach a program that spends its time inside the
 * C functions report H8 found hanging (section 6, item 10). Each runs
 * on a thread; the test cancels it and waits up to 10 seconds for
 * engine_run to return. */
typedef struct {
    engine *e;
    int result;
    bool done;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} cancel_job;

static void *cancel_run(void *arg)
{
    cancel_job *job = arg;
    int r = engine_run(job->e, "<cancel>");
    pthread_mutex_lock(&job->lock);
    job->result = r;
    job->done = true;
    pthread_cond_broadcast(&job->cond);
    pthread_mutex_unlock(&job->lock);
    return NULL;
}

/* Waits up to `ms` for the job to finish. True when it did. */
static bool cancel_wait(cancel_job *job, long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&job->lock);
    while (!job->done) {
        if (pthread_cond_timedwait(&job->cond, &job->lock, &ts) != 0) break;
    }
    bool done = job->done;
    pthread_mutex_unlock(&job->lock);
    return done;
}

TEST cancel_stops_programs_busy_in_container_code(void)
{
    static const char *const bodies[] = {
        "while (true) { x = 1; }",
        "while (true) { l = [1, 2]; l.removeAll(l); }",
        "l = []; l.add(l); while (true) { try { l.toJson(); } catch (ex) { } }",
        "m = {}; m['m'] = m; while (true) { try { m.pack(); } catch (ex) { } }",
    };
    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        size_t n = strlen(bodies[i]) + 96;
        char *src = malloc(n);
        snprintf(src, n, "class Main { public Main() { } public main() { %s } }", bodies[i]);
        engine *e = quiet_engine();
        ASSERT(aussom_parse_string(e, src, "<cancel>"));
        free(src);

        cancel_job *job = malloc(sizeof *job);
        job->e = e;
        job->result = -1;
        job->done = false;
        pthread_mutex_init(&job->lock, NULL);
        pthread_cond_init(&job->cond, NULL);
        pthread_t t;
        ASSERT_EQ(0, pthread_create(&t, NULL, cancel_run, job));

        /* Let it get going, then stop it. */
        ASSERT_FALSEm(bodies[i], cancel_wait(job, 200));
        engine_cancel(e);
        if (!cancel_wait(job, 10000)) {
            /* Still running: leave the thread and the job behind. */
            pthread_detach(t);
            FAILm(bodies[i]);
        }
        pthread_join(t, NULL);
        ASSERT_EQ_FMTm(bodies[i], 1, job->result, "%d");
        free(job);
    }
    PASS();
}

/* A host must be able to pause or cancel a single regular expression
 * match while it runs, not only between matches. The pattern below
 * backtracks for most of a minute under the budget set here, all in
 * one match. */
static const char *const long_match_src =
    "include util; class Main { public Main() { } public main() { "
    "s = \"\"; for (i = 0; i < 24; i++) { s += \"x\"; } "
    "return (s + \"w\").matches(\"(x+x+)+\"); } }";

static cancel_job *long_match_start(engine *e, pthread_t *t)
{
    sm_put(e, ENGINE_LIMIT_REGEX_STEPS_PROP, &av_int_new(2000000000)->base.base);
    engine_refresh_limits(e);
    cancel_job *job = malloc(sizeof *job);
    job->e = e;
    job->result = -1;
    job->done = false;
    pthread_mutex_init(&job->lock, NULL);
    pthread_cond_init(&job->cond, NULL);
    pthread_create(t, NULL, cancel_run, job);
    return job;
}

TEST cancel_stops_a_single_long_regex_match(void)
{
    engine *e = quiet_engine();
    ASSERT(aussom_parse_string(e, long_match_src, "<cancel>"));
    pthread_t t;
    cancel_job *job = long_match_start(e, &t);
    ASSERT_FALSE(cancel_wait(job, 300));
    engine_cancel(e);
    if (!cancel_wait(job, 2000)) { pthread_detach(t); FAIL(); }
    pthread_join(t, NULL);
    ASSERT_EQ_FMT(1, job->result, "%d");
    free(job);
    PASS();
}

TEST pause_parks_a_single_long_regex_match(void)
{
    engine *e = quiet_engine();
    ASSERT(aussom_parse_string(e, long_match_src, "<pause>"));
    pthread_t t;
    cancel_job *job = long_match_start(e, &t);
    ASSERT_FALSE(cancel_wait(job, 300));
    engine_pause(e);
    ASSERT(engine_await_paused(e, 2000));
    engine_cancel(e);
    if (!cancel_wait(job, 2000)) { pthread_detach(t); FAIL(); }
    pthread_join(t, NULL);
    ASSERT_EQ_FMT(1, job->result, "%d");
    free(job);
    PASS();
}

SUITE(security_gates)
{
    RUN_TEST(sys_view_gates_deny_by_default_and_allow_when_set);
    RUN_TEST(reflect_eval_string_gate);
    RUN_TEST(reflect_eval_file_gate);
    RUN_TEST(reflect_clear_diagnostics_gate);
    RUN_TEST(aussomdoc_class_get_json_gate);
    RUN_TEST(securitymanager_gates);
    RUN_TEST(test_runner_gate);
    RUN_TEST(test_mock_inject_gate);
    RUN_TEST(test_mock_spy_gate);
    RUN_TEST(extern_allowlist_gate);
#if !defined(_WIN32)
    RUN_TEST(include_symlink_follow_gate);
#endif
    RUN_TEST(script_mode_gate);
    RUN_TEST(an_unknown_string_escape_is_a_lex_error);
    RUN_TEST(an_illegal_character_reports_java_columns);
    RUN_TEST(cancel_stops_programs_busy_in_container_code);
    RUN_TEST(cancel_stops_a_single_long_regex_match);
    RUN_TEST(pause_parks_a_single_long_regex_match);
}

/* The security manager (report M2): a host's manager is held, not
 * copied, is in force while lang.aus is parsed, and a subclass override
 * reaches every decision, as with Java's SecurityManagerInt. */

/* A host "subclass" of SecurityManagerImpl that counts reads and can
 * answer one property itself. */
typedef struct host_sm {
    security_manager base;
    int retain_reads;
    const char *grant;      /* answered true, whatever props holds */
} host_sm;

static av_type *host_get_property(security_manager *sm, const char *name)
{
    host_sm *h = (host_sm *)sm;
    if (strcmp(name, "aussomdoc.retain") == 0) h->retain_reads++;
    if (h->grant != NULL && strcmp(name, h->grant) == 0) {
        return &av_bool_new(true)->base.base;
    }
    return security_manager_impl_get_property(sm, name);
}

static const security_manager_int host_sm_methods = {
    .get_property         = host_get_property,
    .get_property_boolean = security_manager_impl_get_property_boolean,
    .get_property_int     = security_manager_impl_get_property_int,
    .get_property_double  = security_manager_impl_get_property_double,
    .get_property_string  = security_manager_impl_get_property_string,
    .get_property_list    = security_manager_impl_get_property_list,
    .get_property_map     = security_manager_impl_get_property_map,
    .get_prop             = security_manager_impl_get_prop,
    .key_set              = security_manager_impl_key_set,
    .get_map              = security_manager_impl_get_map,
    .set_prop             = security_manager_impl_set_prop,
    .set_map              = security_manager_impl_set_map,
};

static host_sm *host_sm_new(const char *grant)
{
    host_sm *h = AUSSOM_ALLOC(sizeof(*h));
    security_manager_impl_init(&h->base);
    h->base.methods = &host_sm_methods;
    h->retain_reads = 0;
    h->grant = grant;
    return h;
}

/* Runs `body` as main's body on `e`. Returns main's return value, or -1
 * when the source does not parse. */
static int run_body(engine *e, const char *head, const char *body)
{
    engine_set_logger(e, &silent);
    size_t n = strlen(head) + strlen(body) + 128;
    char *src = malloc(n);
    snprintf(src, n, "%s\nclass Main { public Main() { } public main() {\n%s\n} }", head, body);
    bool ok = aussom_parse_string(e, src, "<sm>");
    free(src);
    if (!ok) return -1;
    return engine_run(e, "<sm>");
}

TEST the_engine_holds_the_host_manager_from_the_lang_parse_on(void)
{
    host_sm *h = host_sm_new(NULL);
    engine *e = engine_new_with_security(&h->base);
    ASSERT_EQ(&h->base, engine_get_security_manager(e));
    /* lang.aus has doc comments, and each one reads aussomdoc.retain. */
    ASSERT(h->retain_reads > 0);
    PASS();
}

TEST a_get_property_override_reaches_the_gates(void)
{
    const char *body = GATE_CALL("reflect.evalStr(\"class smq { public smq() { } }\");");
    ASSERT_EQ(GATE_DENIED, run_body(engine_new_with_security(&host_sm_new(NULL)->base),
                                    "include reflect;", body));
    ASSERT_EQ(GATE_ALLOWED,
              run_body(engine_new_with_security(&host_sm_new("reflect.eval.string")->base),
                       "include reflect;", body));
    PASS();
}

TEST typed_reads_never_convert(void)
{
    security_manager *sm = security_manager_impl_new();
    av_strmap_put(sm->props, "t.str", &av_string_new("true")->base.base);
    av_strmap_put(sm->props, "t.int", &av_int_new(7)->base.base);
    av_strmap_put(sm->props, "t.dbl", &av_double_new(1.5)->base.base);
    ASSERT_FALSE(security_manager_get_property_boolean(sm, "t.str", false));
    ASSERT_EQ(7, security_manager_get_property_int(sm, "t.int", 3));
    ASSERT_EQ(3, security_manager_get_property_int(sm, "t.dbl", 3));
    ASSERT(security_manager_get_property_double(sm, "t.int", 2.5) == 2.5);
    ASSERT(security_manager_get_property_double(sm, "t.dbl", 2.5) == 1.5);
    ASSERT_STR_EQ("true", security_manager_get_property_string(sm, "t.str", "x"));
    ASSERT_STR_EQ("x", security_manager_get_property_string(sm, "t.int", "x"));
    ASSERT_EQ(NULL, security_manager_get_property_list(sm, "t.int"));
    ASSERT_EQ(NULL, security_manager_get_property_map(sm, "t.int"));
    /* The list read is a copy: changing it leaves policy alone. */
    av_list *l = security_manager_get_property_list(sm, "aussom.extern.allowed");
    ASSERT(l != NULL);
    size_t n = av_list_size(l);
    av_list_add(l, &av_string_new("com.evil.*")->base.base);
    ASSERT_EQ(n, av_list_size(security_manager_get_property_list(sm, "aussom.extern.allowed")));
    PASS();
}

/* The managers Main builds: DefaultSecurityManagerImpl and
 * TestSecurityManagerImpl differ from SecurityManagerImpl only in the
 * properties their constructors set. */
TEST the_cli_managers_match_java(void)
{
    security_manager *b = security_manager_impl_new();
    security_manager *d = default_security_manager_impl_new();
    security_manager *t = test_security_manager_impl_new();
    ASSERT_FALSE(security_manager_get_property_boolean(b, "aussomdoc.class.getJson", true));
    ASSERT(security_manager_get_property_boolean(d, "aussomdoc.class.getJson", false));
    ASSERT(security_manager_get_property_boolean(d, "aussomdoc.file.getJson", false));
    ASSERT_FALSE(security_manager_get_property_boolean(d, "test.mock.spy", true));
    static const char *const test_on[] = {
        "aussomdoc.file.getJson", "aussomdoc.class.getJson", "test.mock.inject",
        "test.mock.spy", "aussom.script.mode.enable", "aussom.debugger.enable",
    };
    for (size_t i = 0; i < sizeof test_on / sizeof test_on[0]; i++) {
        ASSERT(security_manager_get_property_boolean(t, test_on[i], false));
    }
    ASSERT_FALSE(security_manager_get_property_boolean(t, "test.aussom.runner", true));
    test_security_manager_impl_set_allow_aussom_test_runner(t, true);
    ASSERT(security_manager_get_property_boolean(t, "test.aussom.runner", false));
    PASS();
}

/* testRunner's child engine runs under the parent's manager object, so
 * a property the child sets is the parent's. */
TEST the_test_runner_child_shares_the_parent_manager(void)
{
    security_manager *sm = test_security_manager_impl_new();
    test_security_manager_impl_set_allow_aussom_test_runner(sm, true);
    av_strmap_put(sm->props, "securitymanager.property.set", &av_bool_new(true)->base.base);
    engine *e = engine_new_with_security(sm);
    int r = run_body(e, "include aunit;",
        "tr = new testRunner();\n"
        "tr.loadTestString(\"smchild\", \"class smchild { public smchild() { } "
        "@Test(name = \\\"t\\\") public t() { secman.setProp('shared.flag', 5); return true; } }\");\n"
        "tr.runClassTests(\"smchild\");\n"
        "return secman.getProp('shared.flag');");
    ASSERT_EQ(5, r);
    PASS();
}

SUITE(security_manager_suite)
{
    RUN_TEST(the_engine_holds_the_host_manager_from_the_lang_parse_on);
    RUN_TEST(a_get_property_override_reaches_the_gates);
    RUN_TEST(typed_reads_never_convert);
    RUN_TEST(the_cli_managers_match_java);
    RUN_TEST(the_test_runner_child_shares_the_parent_manager);
}

/* Map hashing (report M1). Keys a script controls, such as the keys of
 * a JSON object passed to json.parse, must not be choosable so that they
 * all land in one bucket. These keys all share the low 12 bits of their
 * plain FNV-1a hash, the hash maps used before. A map of 4096 buckets
 * put every one of them in a single chain. */
static uint64_t mh_fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

TEST chosen_colliding_keys_spread_across_buckets(void)
{
    enum { KEYS = 3000 };
    av_strmap *m = av_strmap_new();
    char key[32];
    int found = 0;
    for (uint32_t i = 0; found < KEYS; i++) {
        snprintf(key, sizeof(key), "k%08x", i);
        if ((mh_fnv1a(key) & 0xfff) != 0x123) continue;
        av_strmap_put(m, key, NULL);
        found++;
    }
    ASSERT_EQ((size_t)KEYS, av_strmap_size(m));
    ASSERT_EQ((size_t)4096, m->bucket_count);

    size_t longest = 0;
    for (size_t b = 0; b < m->bucket_count; b++) {
        size_t n = 0;
        for (av_strmap_entry *e = m->buckets[b]; e != NULL; e = e->next) n++;
        if (n > longest) longest = n;
    }
    /* A random spread of 3000 keys over 4096 buckets gives a longest
     * chain of about 6. */
    ASSERT_LT(longest, 32);
    PASS();
}

SUITE(map_hash)
{
    RUN_TEST(chosen_colliding_keys_spread_across_buckets);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    /* Must run on this thread before any engine exists. */
    aussom_gc_init();

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(engine_run_suite);
    RUN_SUITE(include_paths);
    RUN_SUITE(load_order);
    RUN_SUITE(stack_guard);
    RUN_SUITE(map_hash);
    RUN_SUITE(security_gates);
    RUN_SUITE(security_manager_suite);
    GREATEST_MAIN_END();
}
