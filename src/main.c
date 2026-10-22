/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Command-line driver. Mirrors the aussom-base CLI (com.aussom.Main):
 *
 *   aussomc [options] <aussom-file>
 *
 *   -h, --help       show the help message and exit
 *   -d, --doc        generate aussomdoc for file
 *   -t, --test       run tests for file
 *   -a, --test-all   run tests for all classes loaded in the engine
 *
 * With no flags a single script file is run: its first class declaring
 * a `main` function is instantiated and `main` is called. All memory is
 * GC-managed; no cleanup needed.
 *
 * A few developer-only flags (`--av-test`, `--eval <src>`, `--dump
 * <file>`) are handled before argument parsing and are not part of the
 * documented interface.
 */

#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "argparse.h"

#include "aussom/ast.h"
#include "aussom/ast_eval.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/callstack.h"
#include "aussom/console.h"
#include "aussom/engine.h"
#include "aussom/environment.h"
#include "aussom/parser.h"
#include "aussom/security_manager.h"
#include "aussom/test_runner.h"
#include "aussom/version.h"

/* --- AST dump (developer flag --dump) ---------------------------- */

static void dump_engine(engine *eng)
{
    size_t inc_count = engine_include_count(eng);
    for (size_t i = 0; i < inc_count; i++) {
        printf("include: %s\n", engine_get_include(eng, i));
    }
    size_t cls_count = engine_class_count(eng);
    for (size_t i = 0; i < cls_count; i++) {
        char *dump = ast_node_to_string_alloc(&engine_get_class(eng, i)->base);
        fputs(dump, stdout);
        fputc('\n', stdout);
    }
}

static int parse_and_dump(const char *path)
{
    engine *eng = engine_new();
    bool ok = aussom_parse_file(eng, path);
    dump_engine(eng);
    if (!ok) {
        if (engine_parse_exception(eng) != NULL)
            engine_err(eng, engine_parse_exception_stack_trace(eng));
        fprintf(stderr, "parse failed for '%s'\n", path);
        return 1;
    }
    return 0;
}

/* --- run a script file ------------------------------------------- */

/* True when `cls` declares at least one function named `main`. */
/* Parses and runs a script file. The running itself is
 * engine_run (Engine.run), so the CLI is one host among others rather
 * than the only place a program can be started from. */
static int run_file(const char *path)
{
    engine *eng = engine_new_with_security(default_security_manager_impl_new());
    /* Main.main: an error the parse throws is logged with its message
     * and the run ends with 1. A syntax error is not thrown; run()
     * refuses to start and reports it ("Parse errors were encountered"). */
    aussom_parse_file(eng, path);
    if (engine_parse_exception(eng) != NULL) {
        engine_err(eng, engine_parse_exception_stack_trace(eng));
        return 1;
    }
    return engine_run(eng, path);
}

/* Parses a test file under the test security manager and runs @Test
 * methods. `run_all` selects every @Test class in the engine
 * (--test-all) versus only those defined in `path` (--test). Mirrors
 * Main.runTest. Returns non-zero if any test failed. */
static int run_test_file(const char *path, bool run_all)
{
    /* The -t flag is the operator explicitly asking to run tests, so
     * grant the permission that testRunner objects created within the
     * script require. */
    security_manager *secman = test_security_manager_impl_new();
    test_security_manager_impl_set_allow_aussom_test_runner(secman, true);

    engine *eng = engine_new_with_security(secman);
    /* Main.runTest: an error the parse throws is logged with its
     * message. A syntax error is not thrown: loading the test classes
     * comes first and fails when the file left none, and otherwise
     * runTests refuses to start. */
    aussom_parse_file(eng, path);
    if (engine_parse_exception(eng) != NULL) {
        engine_err(eng, engine_parse_exception_stack_trace(eng));
        return 1;
    }
    if (engine_has_parse_error(eng)) {
        bool found = false;
        size_t n = engine_class_count(eng);
        for (size_t i = 0; i < n && !found; i++) {
            ast_class *cls = engine_get_class(eng, i);
            if (run_all) {
                found = test_class_contains_tests(cls);
            } else {
                const char *fn = cls->base.file_name;
                found = fn != NULL && strcmp(fn, path) == 0;
            }
        }
        /* Thrown, so printed with getAussomStackTrace's newline. */
        engine_err(eng, found
            ? "Engine.runTest(): Parse errors were encountered. Not running.\n"
            : "Engine.runTest(): No classes found for that script file.\n");
        return 1;
    }

    test_result r = run_all ? test_runner_run_all(eng)
                            : test_runner_run_file(eng, path);
    return r.failed == 0 ? 0 : 1;
}

/* --- runtime value-type test (developer flag --av-test) ---------- */

#define AV_TEST_THREADS 8
#define AV_TEST_ITERS   50000

static av_list *g_list;
static av_map  *g_map;

/* Each worker hammers the shared list and map concurrently. With
 * object-level locking the operations stay individually atomic, so
 * the final counts are exact and there are no torn reads/crashes. */
static void *av_test_worker(void *arg)
{
    /* The thread index is smuggled through the void * argument, so it
     * has to round-trip through an integer that is the width of a
     * pointer. long is not that on Windows, where it stays 32 bits
     * while pointers are 64. */
    intptr_t id = (intptr_t)arg;
    char key[32];
    for (int i = 0; i < AV_TEST_ITERS; i++) {
        av_list_add(g_list, &av_int_new((int64_t)id * AV_TEST_ITERS + i)->base.base);
        snprintf(key, sizeof(key), "t%" PRIdPTR "-%d", id, i);
        av_map_put(g_map, key, &av_int_new(i)->base.base);
        if ((i & 0x3F) == 0) {
            (void)av_list_size(g_list);
            (void)av_map_size(g_map);
        }
    }
    return NULL;
}

static int av_test(void)
{
    /* Demonstrate the value representation + str/toString. */
    av_list *l = av_list_new();
    av_list_add(l, &av_int_new(42)->base.base);
    av_list_add(l, &av_string_new("hi")->base.base);
    av_list_add(l, &av_bool_new(true)->base.base);

    av_map *m = av_map_new();
    av_map_put(m, "count", &av_int_new(3)->base.base);
    av_map_put(m, "items", &l->base.base);
    av_map_put(m, "ratio", &av_double_new(0.5)->base.base);

    char *s = av_str((const av_type *)m);
    printf("map str():\n%s\n\n", s);
    char *j = av_to_string_alloc((const av_type *)m);
    printf("map toString():\n%s\n", j);

    /* Concurrency: hammer a shared list + map from many threads. */
    g_list = av_list_new();
    g_map = av_map_new();
    pthread_t t[AV_TEST_THREADS];
    for (intptr_t i = 0; i < AV_TEST_THREADS; i++) {
        pthread_create(&t[i], NULL, av_test_worker, (void *)i);
    }
    for (int i = 0; i < AV_TEST_THREADS; i++) pthread_join(t[i], NULL);

    size_t expect = (size_t)AV_TEST_THREADS * AV_TEST_ITERS;
    size_t got_list = av_list_size(g_list);
    size_t got_map = av_map_size(g_map);
    printf("\nconcurrent: list=%zu map=%zu expected=%zu\n",
           got_list, got_map, expect);
    if (got_list != expect || got_map != expect) {
        fprintf(stderr, "av-test FAILED: lost updates under concurrency\n");
        return 1;
    }
    printf("av-test OK\n");
    return 0;
}

/* --- script evaluation (developer flag --eval) ------------------- */

/* Runs `src` in script mode (bare top-level statements), then prints
 * the resulting locals. */
static int eval_script(const char *src)
{
    /* --eval is script mode, which is off by default. */
    security_manager *secman = default_security_manager_impl_new();
    av_strmap_put(secman->props, "aussom.script.mode.enable",
                  &av_bool_new(true)->base.base);
    engine *eng = engine_new_with_security(secman);
    const char *err = engine_set_script_mode(eng, true);
    if (err != NULL) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    engine_set_script_file_name(eng, "<eval>");
    ast_statement_list *script = NULL;
    if (engine_parse_script_line(eng, src, 1, &script) != NULL) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }

    av_type *res = engine_eval_parsed_script(eng, script);
    if (av_is_ex(res)) {
        fputs("exception:\n", stderr);
        av_text_write_out(av_str(res), stderr);
        fputc('\n', stderr);
        return 1;
    }

    av_strmap *locals = environment_locals(engine_get_script_environment(eng));
    av_strmap_lock(locals);
    size_t n = av_strmap_size_locked(locals);
    for (size_t i = 0; i < n; i++) {
        const char *k = av_strmap_key_at_locked(locals, i);
        av_type *v = av_strmap_value_at_locked(locals, i);
        av_strmap_unlock(locals);
        printf("%s = %s\n", k, av_str(v));
        av_strmap_lock(locals);
    }
    av_strmap_unlock(locals);
    return 0;
}

/* --- command line ------------------------------------------------ */

static const char *const cli_usages[] = {
    "aussomc [options] <aussom-file>",
    NULL,
};

static int arg_count_error(struct argparse *ap, int nargs)
{
    if (nargs < 1) {
        fprintf(stderr, "Expecting aussom script file, but no arguments found.\n\n");
    } else {
        fprintf(stderr, "Too many arguments provided, expecting just one script file.\n\n");
    }
    argparse_usage(ap);
    return 1;
}

int main(int argc, char **argv)
{
    aussom_gc_init();

    /* Developer-only flags (not part of the public CLI). Handled before
     * argparse so the documented interface mirrors aussom-base. */
    if (argc > 1 && strcmp(argv[1], "--av-test") == 0) return av_test();
    if (argc == 3 && strcmp(argv[1], "--eval") == 0) return eval_script(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) return parse_and_dump(argv[2]);

    int doc = 0, test = 0, test_all = 0;
    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("Options"),
        /* The trailing NULL, 0, 0 fill callback/data/flags so the
         * aggregate initializer is complete (-Wmissing-field-initializers). */
        OPT_BOOLEAN('d', "doc", &doc, "generate aussomdoc for file", NULL, 0, 0),
        OPT_BOOLEAN('t', "test", &test, "run tests for file", NULL, 0, 0),
        OPT_BOOLEAN('a', "test-all", &test_all,
                    "run tests for all classes loaded in the engine", NULL, 0, 0),
        OPT_END(),
    };
    struct argparse ap;
    argparse_init(&ap, options, cli_usages, 0);
    argparse_describe(&ap,
        "\nAussom Version " AUSSOM_VERSION_STRING "\n"
        "Copyright 2026 Austin Lehman\n"
        "Apache License Version 2",
        NULL);
    int nargs = argparse_parse(&ap, argc, (const char **)argv);

    if (doc) {
        if (nargs != 1) return arg_count_error(&ap, nargs);
        fprintf(stderr,
            "aussom: aussomdoc generation is not yet implemented in the C port.\n");
        return 1;
    }

    if (test || test_all) {
        if (nargs != 1) return arg_count_error(&ap, nargs);
        char msg[512];
        snprintf(msg, sizeof(msg), "Running %stests for file '%s'.",
                 test_all ? "all " : "", argv[0]);
        console_info(msg);
        return run_test_file(argv[0], test_all);
    }

    if (nargs != 1) return arg_count_error(&ap, nargs);
    return run_file(argv[0]);
}
