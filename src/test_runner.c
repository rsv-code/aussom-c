/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unit-test runner (com.aussom.UnitTestRunner) plus the `testRunner`
 * extern class (com.aussom.stdlib.ATestRunner). The runner discovers
 * @Test classes and drives each through the per-test hook loop
 * (@Before / @BeforeEach / test / @OnTestFail / @AfterEach / @After);
 * the testRunner class exposes the same machinery to Aussom code over
 * a separately-parsed inner engine.
 */

#include "aussom/test_runner.h"
#include "aussom/security_manager.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aussom/ast_annotation.h"
#include "aussom/ast_annotation_arg.h"
#include "aussom/ast_eval.h"
#include "aussom/ast_funct_def.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/callstack.h"
#include "aussom/console.h"
#include "aussom/environment.h"
#include "aussom/extern_registry.h"
#include "aussom/parser.h"

/* The 62-char banner runTests prints around the header and summary. */
#define TEST_BANNER \
    "**************************************************************"

/* --- annotation introspection ------------------------------------ */

static ast_annotation *func_annotation(ast_funct_def *fd, const char *name)
{
    const ast_node_array *anns = &fd->base.annotations;
    for (size_t i = 0; i < anns->count; i++) {
        ast_annotation *a = (ast_annotation *)anns->items[i];
        if (strcmp(ast_annotation_get_name(a), name) == 0) return a;
    }
    return NULL;
}

/* astClass.getFunct(name, ""): the zero-argument overload the runner
 * calls a test or hook through. */
static ast_funct_def *find_func(ast_class *cls, const char *name)
{
    return ast_class_get_funct(cls, name, "");
}

/* The function named by a hook annotation, as astClass.getTestClass
 * records it from functList: the last function carrying it. */
static const char *hook_func(const ast_class_tables *tl, const char *ann_name)
{
    size_t nf;
    ast_funct_def **fl = ast_class_tables_functions(tl, &nf);
    const char *found = NULL;
    for (size_t i = 0; i < nf; i++) {
        if (func_annotation(fl[i], ann_name) != NULL) found = fl[i]->base.name;
    }
    return found;
}

/* astClass.containsTests. */
bool test_class_contains_tests(ast_class *cls)
{
    size_t nf;
    ast_funct_def **fl = ast_class_get_all_functions(cls, &nf);
    for (size_t i = 0; i < nf; i++) {
        if (func_annotation(fl[i], "Test") != NULL) return true;
    }
    return false;
}

/* The trailing label in "Running Test [ ClassName : <label> ]": the
 * class-level @Test name if set, else "class tests" (matching
 * UnitTestClass.getTestDisplayString). */
static const char *class_test_label(ast_class *cls)
{
    const ast_node_array *anns = &cls->base.annotations;
    for (size_t i = 0; i < anns->count; i++) {
        ast_annotation *a = (ast_annotation *)anns->items[i];
        if (strcmp(ast_annotation_get_name(a), "Test") == 0) {
            const char *nm = ast_annotation_find_value(a, "name");
            if (nm != NULL && nm[0] != '\0') return nm;
        }
    }
    return "class tests";
}

/* The full exception representation used in failure / hook messages,
 * matching the Java runner's use of aussomException.getMessage()
 * (av_str routes exceptions through the multi-line block formatter). */
static const char *exmsg(av_type *e) { return av_str(e); }

/* Parses a @Test tags="a, b" arg into a list of trimmed, non-empty
 * tag strings. */
static av_list *parse_tags(ast_annotation *t)
{
    av_list *out = av_list_new();
    const char *tags = (t != NULL) ? ast_annotation_find_value(t, "tags") : NULL;
    if (tags == NULL) return out;
    const char *p = tags;
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != ',') p++;
        const char *end = p;
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > start) {
            size_t len = (size_t)(end - start);
            char *tag = AUSSOM_ALLOC_ATOMIC(len + 1);
            memcpy(tag, start, len);
            tag[len] = '\0';
            av_list_add(out, &av_string_new(tag)->base.base);
        }
        if (*p == ',') p++;
    }
    return out;
}

static bool list_has(av_list *l, const char *s)
{
    if (l == NULL) return false;
    for (size_t i = 0; i < av_list_size(l); i++) {
        av_type *v = av_list_get(l, i);
        if (v->type == C_TYPE_STRING && strcmp(av_string_get((av_string *)v), s) == 0) return true;
    }
    return false;
}

/* Tag include/exclude filter (exclude wins; an include filter excludes
 * untagged tests). */
static bool tags_should_run(av_list *inc, av_list *exc, av_list *tags)
{
    if (exc != NULL && av_list_size(exc) > 0) {
        for (size_t i = 0; i < av_list_size(tags); i++) {
            if (list_has(exc, av_string_get((av_string *)av_list_get(tags, i)))) return false;
        }
    }
    if (inc != NULL && av_list_size(inc) > 0) {
        for (size_t i = 0; i < av_list_size(tags); i++) {
            if (list_has(inc, av_string_get((av_string *)av_list_get(tags, i)))) return true;
        }
        return false;
    }
    return true;
}

/* --- invocation -------------------------------------------------- */

static av_type *run_func(engine *eng, av_object *inst, ast_class *cls, const char *fname)
{
    environment *env = environment_new(eng);
    callstack *root = callstack_new();
    environment_set(env, NULL, av_strmap_new_unlocked(), root);
    environment *tenv = environment_clone(env, &inst->base);
    return class_call(tenv, false, cls, fname, av_list_new());
}

/* --- per-class run core ------------------------------------------ */

/* Runs every @Test in `cls` (in `eng`) through the hook loop
 * (UnitTestRunner.runClass). `inc`/`exc` are optional tag filters;
 * `quiet` suppresses stdout.
 *
 * An exception from @Before or @After ends the run, as it escapes
 * runClass in Java: *thrown is set to it and the counts are dropped
 * (a zero result), so a @Before failure runs none of the tests. The
 * caller reports it. */
static test_result run_class_core(engine *eng, ast_class *cls,
                                  const ast_class_tables *tl, av_list *inc,
                                  av_list *exc, bool quiet, av_type **thrown)
{
    *thrown = NULL;
    test_result r = { 0, 0, 0, 0 };

    /* Java builds the test class (UnitTestClass) from functList before
     * the class is instantiated, so the tests and hooks are the ones
     * listed then. `tl` is the table read when the class was loaded,
     * or NULL to read it now. */
    if (tl == NULL) tl = ast_class_tables_of(cls);
    size_t nf;
    ast_funct_def **fl = ast_class_tables_functions(tl, &nf);

    environment *env = environment_new(eng);
    callstack *root = callstack_new();
    environment_set(env, NULL, av_strmap_new_unlocked(), root);
    av_type *instv = class_instantiate(env, cls, false, av_list_new());
    if (av_is_ex(instv)) {
        if (!quiet) engine_err(eng, exmsg(instv));
        return r;
    }
    av_object *inst = (av_object *)instv;

    const char *before     = hook_func(tl, "Before");
    const char *beforeEach = hook_func(tl, "BeforeEach");
    const char *afterEach  = hook_func(tl, "AfterEach");
    const char *onFail     = hook_func(tl, "OnTestFail");
    const char *after      = hook_func(tl, "After");

    char emsg[8192];

    if (before != NULL) {
        av_type *b = run_func(eng, inst, cls, before);
        if (av_is_ex(b)) {
            *thrown = b;
            return (test_result){ 0, 0, 0, 0 };
        }
    }

    /* getTestClass adds one test for each @Test annotation, so a
     * function annotated twice runs twice. */
    for (size_t i = 0; i < nf; i++) {
        ast_funct_def *fd = fl[i];
        const ast_node_array *anns = &fd->base.annotations;
        for (size_t ai = 0; ai < anns->count; ai++) {
            ast_annotation *t = (ast_annotation *)anns->items[ai];
            if (strcmp(ast_annotation_get_name(t), "Test") != 0) continue;

            const char *fname = fd->base.name;
            const char *tname = ast_annotation_find_value(t, "name");
            if (tname == NULL || tname[0] == '\0') tname = "unit test";
            r.total++;

            /* "*** fname : tname ... " — the result token is appended below. */
            char line[8192];
            int n = snprintf(line, sizeof(line), "*** %s : %s ... ", fname, tname);
            if (n < 0) n = 0;
            size_t off = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;

            const char *skip = ast_annotation_find_value(t, "skip");
            if (skip != NULL && strcmp(skip, "true") == 0) {
                r.skipped++;
                if (!quiet) {
                    snprintf(line + off, sizeof(line) - off, "SKIPPED");
                    engine_info(eng, line);
                }
                continue;
            }
            if (!tags_should_run(inc, exc, parse_tags(t))) {
                r.skipped++;
                if (!quiet) {
                    snprintf(line + off, sizeof(line) - off, "SKIPPED (filtered)");
                    engine_info(eng, line);
                }
                continue;
            }

            bool passed = true;
            char failmsg[4096];
            failmsg[0] = '\0';

            bool before_each_ok = true;
            if (beforeEach != NULL) {
                av_type *be = run_func(eng, inst, cls, beforeEach);
                if (av_is_ex(be)) {
                    before_each_ok = false;
                    passed = false;
                    snprintf(failmsg, sizeof(failmsg), "@BeforeEach threw: %s", exmsg(be));
                }
            }

            if (before_each_ok) {
                av_type *res = run_func(eng, inst, cls, fname);
                if (av_is_ex(res)) {
                    passed = false;
                    snprintf(failmsg, sizeof(failmsg), "%s", exmsg(res));
                } else if (res->type == C_TYPE_INT && av_int_get((av_int *)res) != 0) {
                    passed = false;
                    snprintf(failmsg, sizeof(failmsg), "test returned non-zero");
                }
            }

            if (!passed && onFail != NULL) {
                av_type *of = run_func(eng, inst, cls, onFail);
                if (av_is_ex(of) && !quiet) {
                    snprintf(emsg, sizeof(emsg), "@OnTestFail threw: %s", exmsg(of));
                    engine_err(eng, emsg);
                }
            }
            if (afterEach != NULL) {
                av_type *ae = run_func(eng, inst, cls, afterEach);
                if (av_is_ex(ae) && !quiet) {
                    snprintf(emsg, sizeof(emsg), "@AfterEach threw: %s", exmsg(ae));
                    engine_err(eng, emsg);
                }
            }

            if (passed) {
                r.passed++;
                if (!quiet) {
                    snprintf(line + off, sizeof(line) - off, "PASSED");
                    engine_info(eng, line);
                }
            } else {
                r.failed++;
                if (!quiet) {
                    if (failmsg[0] != '\0')
                        snprintf(line + off, sizeof(line) - off, "FAILED\n%s", failmsg);
                    else
                        snprintf(line + off, sizeof(line) - off, "FAILED");
                    engine_info(eng, line);
                }
            }
        }
    }

    if (after != NULL) {
        av_type *a = run_func(eng, inst, cls, after);
        if (av_is_ex(a)) {
            *thrown = a;
            return (test_result){ 0, 0, 0, 0 };
        }
    }
    return r;
}

/* runClass as UnitTestRunner.runTestClasses calls it: an exception from
 * @Before or @After is logged and the class adds nothing to the
 * totals. */
static test_result run_class_logged(engine *eng, ast_class *cls)
{
    av_type *thrown = NULL;
    test_result r = run_class_core(eng, cls, NULL, NULL, NULL, false, &thrown);
    if (thrown != NULL) engine_err(eng, exmsg(thrown));
    return r;
}

/* The blank line + "Running Test [ ... ]" header the outer test loop
 * prints before each class (UnitTestRunner.runTestClasses). runClass
 * itself does not print this, so the in-Aussom testRunner -- which calls
 * the per-class core directly -- omits it, matching Java. */
static void print_class_header(engine *eng, ast_class *cls)
{
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "Running Test [ %s : %s ]",
             cls->base.name, class_test_label(cls));
    engine_log(eng, "");
    engine_info(eng, hdr);
}

test_result test_runner_run_class(engine *eng, ast_class *cls)
{
    print_class_header(eng, cls);
    return run_class_logged(eng, cls);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The header runTests prints before the per-class output. */
static void print_test_header(engine *eng)
{
    engine_log(eng, "");
    engine_info(eng, TEST_BANNER);
    engine_info(eng, "RUNNING TESTS");
    engine_info(eng, TEST_BANNER);
}

/* The summary runTests prints after all classes, including elapsed
 * wall-clock time. */
static void print_test_summary(engine *eng, test_result total, double elapsed)
{
    char line[256];
    engine_log(eng, "");
    engine_info(eng, TEST_BANNER);
    snprintf(line, sizeof(line), "PASSED: %d SKIPPED: %d FAILED: %d TOTAL: %d",
             total.passed, total.skipped, total.failed, total.total);
    engine_info(eng, line);
    snprintf(line, sizeof(line), "Elapsed: %.3fs", elapsed);
    engine_info(eng, line);
    engine_info(eng, TEST_BANNER "\n");
}

/* Main.runTest prints a thrown aussomException as getAussomStackTrace():
 * the message, a newline and the (here empty) trace. */
static void log_thrown(engine *eng, const char *msg)
{
    size_t n = strlen(msg) + 2;
    char *m = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(m, n, "%s\n", msg);
    engine_err(eng, m);
}

/* UnitTestRunner.loadAllTestClasses then runTests: the test classes
 * are gathered first, and with none the run stops with a thrown error
 * before the header is printed. */
test_result test_runner_run_all(engine *eng)
{
    test_result total = { 0, 0, 0, 0 };
    size_t n = engine_class_count(eng);
    bool any = false;
    for (size_t i = 0; i < n && !any; i++)
        any = test_class_contains_tests(engine_get_class(eng, i));
    if (!any) {
        log_thrown(eng, "Engine.runTest(): No classes found for that script file.");
        total.failed = 1;
        return total;
    }
    print_test_header(eng);
    double start = now_seconds();
    for (size_t i = 0; i < n; i++) {
        ast_class *cls = engine_get_class(eng, i);
        if (!test_class_contains_tests(cls)) continue;
        print_class_header(eng, cls);
        test_result r = run_class_logged(eng, cls);
        total.total += r.total;
        total.passed += r.passed;
        total.skipped += r.skipped;
        total.failed += r.failed;
    }
    print_test_summary(eng, total, now_seconds() - start);
    return total;
}

/* UnitTestRunner.loadTestClasses then runTests: a file with no classes
 * at all is a thrown error before the header. A file whose classes hold
 * no tests runs nothing and passes. */
test_result test_runner_run_file(engine *eng, const char *path)
{
    test_result total = { 0, 0, 0, 0 };
    size_t n = engine_class_count(eng);
    bool any = false;
    for (size_t i = 0; i < n && !any; i++) {
        const char *fn = engine_get_class(eng, i)->base.file_name;
        any = fn != NULL && strcmp(fn, path) == 0;
    }
    if (!any) {
        log_thrown(eng, "Engine.runTest(): No classes found for that script file.");
        total.failed = 1;
        return total;
    }
    print_test_header(eng);
    double start = now_seconds();
    for (size_t i = 0; i < n; i++) {
        ast_class *cls = engine_get_class(eng, i);
        const char *fn = cls->base.file_name;
        if (fn == NULL || strcmp(fn, path) != 0) continue;
        if (!test_class_contains_tests(cls)) continue;
        print_class_header(eng, cls);
        test_result r = run_class_logged(eng, cls);
        total.total += r.total;
        total.passed += r.passed;
        total.skipped += r.skipped;
        total.failed += r.failed;
    }
    print_test_summary(eng, total, now_seconds() - start);
    return total;
}

/* --- testRunner extern class (ATestRunner) ----------------------- */

/* The tag is how runner_state tells its own state from anything else:
 * a script-written binding or a subclass leaves extern_object pointing
 * at an object, and reading that as a trunner would use its fields as
 * pointers. First field, so the read is always in
 * bounds. */
#define TRUNNER_MAGIC 0x5452554eu   /* "TRUN" */

typedef struct trunner {
    uint32_t magic;
    /* The runner engine (UnitTestRunner). Built once by the constructor;
     * every loadTestFile / loadTestString parses into it. */
    engine *eng;
    /* Names of the loaded test classes, in load order (testClasses).
     * Loading a file twice lists its classes twice, as in Java. */
    av_list *test_classes;
    /* Each loaded test class's table as it was at load, which is when
     * Java builds its UnitTestClass. */
    av_strmap *test_tables;
    av_list *inc;    /* include-tag filter, or NULL */
    av_list *exc;    /* exclude-tag filter, or NULL */
    /* The environment runBefore, runTest and runAfter run in (tenv). Its
     * current object carries from runBefore into runTest and runAfter. */
    environment *tenv;
    /* classObjects. getClassObject reads it, but, as in Java, nothing
     * ever stores into it, so every lookup builds a new object. */
    av_strmap *class_objects;
    /* The engine that constructed this runner. The runner engine runs
     * under its security manager, so a test engine can never do
     * anything the engine that asked for it could not. */
    engine *parent;
} trunner;

/* Lazily attaches runner state to the testRunner instance. A fresh
 * instance has extern_object pointing at itself; a subclass may carry a
 * copied parent object instead. Anything without the tag gets fresh
 * state with no runner engine, which is Java's ATestRunner before
 * newTestRunner has run (runner is null). */
static trunner *runner_state(av_type *self)
{
    av_object *o = (av_object *)self;
    void *eo = av_object_get_extern_object(o);
    if (eo == NULL || eo == (void *)o ||
        ((trunner *)eo)->magic != TRUNNER_MAGIC) {
        trunner *t = AUSSOM_ALLOC(sizeof(trunner));
        t->magic = TRUNNER_MAGIC;
        t->eng = NULL;
        t->test_classes = av_list_new();
        t->test_tables = av_strmap_new();
        t->inc = NULL;
        t->exc = NULL;
        t->tenv = NULL;
        t->class_objects = av_strmap_new();
        t->parent = NULL;
        av_object_set_extern_object(o, t);
        return t;
    }
    return (trunner *)eo;
}

static av_type *tr_exc(const char *m) { return &av_exception_new(m)->base.base; }

/* Builds the runner engine. It runs under the parent's own security
 * manager, the same object rather than a copy, as ATestRunner does with
 * new UnitTestRunner(eng.getSecurityManager()): the child must not be
 * able to do anything the engine that created it could not. */
static engine *new_child_engine(trunner *t)
{
    if (t->parent == NULL) return engine_new();
    return engine_new_with_security(engine_get_security_manager(t->parent));
}

/* ATestRunner.resetEnvironment: a new environment with a fresh call
 * stack and no current object. */
static void reset_environment(trunner *t)
{
    t->tenv = environment_new(t->eng);
    callstack *root = callstack_new();
    environment_set(t->tenv, NULL, av_strmap_new_unlocked(), root);
}

/* The Aussom `testRunner()` constructor, gated on test.aussom.runner as
 * read from the engine that is running.
 *
 * Building a test runner builds a second engine, and that engine used
 * to be created with the unit-test permissions switched on regardless
 * of what the calling engine was allowed to do. Two things fix it: the
 * check here, and having the child run under the parent's manager in
 * new_child_engine rather than granting itself privileges.
 *
 * The check cannot live in runner_state: that runs lazily on first use
 * and has no way to refuse, and a constructor with no environment
 * cannot reach the engine to ask. */
static av_type *tr_newTestRunner(av_type *self, environment *env, av_list *args)
{
    (void)args;
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "test.aussom.runner", false)) {
        return tr_exc("testRunner(): Security exception, action "
                      "'test.aussom.runner' not permitted.");
    }
    trunner *t = runner_state(self);
    t->parent = eng;
    t->eng = new_child_engine(t);
    reset_environment(t);
    return self;
}

/* Guard for every runner method that touches the runner engine. The
 * engine exists only once newTestRunner has passed its permission
 * check. Before that, Java's runner field is null and the method throws
 * a NullPointerException naming the UnitTestRunner call it was making
 * (`call`). */
static av_type *tr_no_runner(trunner *t, const char *call)
{
    if (t->eng != NULL) return NULL;
    char msg[256];
    snprintf(msg, sizeof(msg),
        "java.lang.NullPointerException: Cannot invoke "
        "\"com.aussom.stdlib.UnitTestRunner.%s\" because \"this.runner\" "
        "is null", call);
    return &av_exception_new_thrown(msg)->base.base;
}

#define TR_BY_NAME "getTestClassByName(String)"

/* True when the runner engine holds a class defined in `file_name`. */
static bool file_has_classes(trunner *t, const char *file_name)
{
    size_t n = engine_class_count(t->eng);
    for (size_t i = 0; i < n; i++) {
        const char *fn = engine_get_class(t->eng, i)->base.file_name;
        if (fn != NULL && strcmp(fn, file_name) == 0) return true;
    }
    return false;
}

/* The exception a script sees when an extern method's Java code lets an
 * aussomException escape: astFunctDef's
 * EXTERN_INVOCATION_TARGET_EXCEPTION, whose text ends with the escaping
 * exception's toString(), its Java class name and message. */
static av_type *tr_uncaught(environment *env, const char *method,
                                const char *thrown)
{
    size_t n = strlen(method) + strlen(thrown) + 192;
    char *msg = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(msg, n,
        "External call, invocation target exception for method '%s', the "
        "external method threw an uncaught exception: "
        "com.aussom.ast.aussomException: %s",
        method, thrown);
    return eval_make_exception(env, 0, "EXTERN_INVOCATION_TARGET_EXCEPTION", msg);
}

/* After parsing `file_name`: UnitTestRunner.loadTestClasses, which adds
 * the file's classes that contain tests and throws when the file has no
 * classes at all. That throw leaves the extern method, so it reaches
 * the script wrapped the same way a thrown parse error does.
 *
 * A parse error Java throws (an extern function that is not public, a
 * duplicate overload, ...) escapes the load before loadTestClasses
 * runs. A syntax error does not: Java logs it, stops the parse and
 * carries on, so whatever classes were read before it are loaded. */
static av_type *load_test_classes(trunner *t, environment *env,
                                  const char *file_name, const char *method)
{
    const char *thrown = engine_parse_exception(t->eng);
    if (thrown != NULL) return tr_uncaught(env, method, thrown);
    if (!file_has_classes(t, file_name)) {
        return tr_uncaught(env, method,
            "Engine.runTest(): No classes found for that script file.");
    }
    size_t n = engine_class_count(t->eng);
    for (size_t i = 0; i < n; i++) {
        ast_class *cls = engine_get_class(t->eng, i);
        const char *fn = cls->base.file_name;
        if (fn != NULL && strcmp(fn, file_name) == 0 && test_class_contains_tests(cls)) {
            av_list_add(t->test_classes, &av_string_new(cls->base.name)->base.base);
            if (!av_strmap_contains(t->test_tables, cls->base.name))
                av_strmap_put(t->test_tables, cls->base.name,
                              (av_type *)ast_class_tables_of(cls));
        }
    }
    return NULL;
}

/* UnitTestRunner.getTestClassByName: the class, when `name` is one of
 * the loaded test classes. */
static ast_class *tr_test_class(trunner *t, const char *name)
{
    if (t->eng == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < av_list_size(t->test_classes); i++) {
        if (strcmp(av_list_get_string(t->test_classes, i), name) == 0) {
            return engine_get_class_by_name(t->eng, name);
        }
    }
    return NULL;
}

/* The table a loaded test class had when it was loaded. */
static const ast_class_tables *tr_test_tables(trunner *t, const char *name)
{
    return (const ast_class_tables *)av_strmap_get(t->test_tables, name);
}

/* "testRunner.<method>(): Provided class '<name>' not found." */
static av_type *tr_class_not_found(const char *method, const char *name)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "testRunner.%s(): Provided class '%s' not found.",
             method, name != NULL ? name : "");
    return tr_exc(msg);
}

/* "testRunner.<method>(): Function '<fn>' not found on class '<name>'." */
static av_type *tr_function_not_found(const char *method, const char *fn,
                                      const char *name)
{
    char msg[512];
    snprintf(msg, sizeof(msg),
             "testRunner.%s(): Function '%s' not found on class '%s'.",
             method, fn != NULL ? fn : "", name != NULL ? name : "");
    return tr_exc(msg);
}

static av_type *tr_loadTestString(av_type *self, environment *env, av_list *args)
{
    trunner *t = runner_state(self);
    av_type *denied = tr_no_runner(t, "parseString(String, String)");
    if (denied != NULL) return denied;
    const char *name = av_list_get_string(args, 0);
    const char *code = av_list_get_string(args, 1);
    aussom_parse_string(t->eng, code, name);
    av_type *err = load_test_classes(t, env, name, "loadTestString");
    return (err != NULL) ? err : self;
}

static av_type *tr_loadTestFile(av_type *self, environment *env, av_list *args)
{
    trunner *t = runner_state(self);
    av_type *denied = tr_no_runner(t, "parseFile(String)");
    if (denied != NULL) return denied;
    const char *file = av_list_get_string(args, 0);
    aussom_parse_file(t->eng, file);
    av_type *err = load_test_classes(t, env, file, "loadTestFile");
    return (err != NULL) ? err : self;
}

static av_type *tr_getTestClasses(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    trunner *t = runner_state(self);
    av_type *denied = tr_no_runner(t, "getTestClasses()");
    if (denied != NULL) return denied;
    av_list *out = av_list_new();
    for (size_t i = 0; i < av_list_size(t->test_classes); i++) {
        av_list_add(out, av_list_get(t->test_classes, i));
    }
    return &out->base.base;
}

static av_type *tr_getTestFunctions(av_type *self, environment *env, av_list *args)
{
    (void)env;
    const char *name = av_list_get_string(args, 0);
    av_type *denied = tr_no_runner(runner_state(self), TR_BY_NAME);
    if (denied != NULL) return denied;
    ast_class *cls = tr_test_class(runner_state(self), name);
    if (cls == NULL) return tr_class_not_found("getTestFunctions", name);
    av_list *out = av_list_new();
    size_t nf;
    ast_funct_def **fl = ast_class_tables_functions(tr_test_tables(runner_state(self), name), &nf);
    for (size_t i = 0; i < nf; i++) {
        const ast_node_array *anns = &fl[i]->base.annotations;
        for (size_t ai = 0; ai < anns->count; ai++) {
            if (strcmp(ast_annotation_get_name((ast_annotation *)anns->items[ai]), "Test") == 0)
                av_list_add(out, &av_string_new(fl[i]->base.name)->base.base);
        }
    }
    return &out->base.base;
}

static av_type *tr_has(av_type *self, av_list *args, const char *method,
                       const char *ann)
{
    const char *name = av_list_get_string(args, 0);
    av_type *denied = tr_no_runner(runner_state(self), TR_BY_NAME);
    if (denied != NULL) return denied;
    ast_class *cls = tr_test_class(runner_state(self), name);
    if (cls == NULL) return tr_class_not_found(method, name);
    return &av_bool_new(hook_func(tr_test_tables(runner_state(self), name), ann) != NULL)->base.base;
}

static av_type *tr_hasBefore(av_type *s, environment *e, av_list *a)     { (void)e; return tr_has(s, a, "hasBefore", "Before"); }
static av_type *tr_hasAfter(av_type *s, environment *e, av_list *a)      { (void)e; return tr_has(s, a, "hasAfter", "After"); }
static av_type *tr_hasBeforeEach(av_type *s, environment *e, av_list *a) { (void)e; return tr_has(s, a, "hasBeforeEach", "BeforeEach"); }
static av_type *tr_hasAfterEach(av_type *s, environment *e, av_list *a)  { (void)e; return tr_has(s, a, "hasAfterEach", "AfterEach"); }
static av_type *tr_hasOnTestFail(av_type *s, environment *e, av_list *a) { (void)e; return tr_has(s, a, "hasOnTestFail", "OnTestFail"); }

/* The @Test annotation of (className, funcName) for `method`, or NULL
 * with *err set to Java's class or function not-found exception. */
static ast_annotation *tr_test_annotation(trunner *t, av_list *args,
                                          const char *method, av_type **err)
{
    const char *name = av_list_get_string(args, 0);
    const char *fn = av_list_get_string(args, 1);
    *err = tr_no_runner(t, TR_BY_NAME);
    if (*err != NULL) return NULL;
    ast_class *cls = tr_test_class(t, name);
    if (cls == NULL) {
        *err = tr_class_not_found(method, name);
        return NULL;
    }
    ast_funct_def *fd = (fn != NULL) ? find_func(cls, fn) : NULL;
    ast_annotation *ta = (fd != NULL) ? func_annotation(fd, "Test") : NULL;
    if (ta == NULL) *err = tr_function_not_found(method, fn, name);
    return ta;
}

static av_type *tr_getTestTags(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_type *err = NULL;
    ast_annotation *ta = tr_test_annotation(runner_state(self), args, "getTestTags", &err);
    if (ta == NULL) return err;
    return &parse_tags(ta)->base.base;
}

static av_type *tr_getTestTimeoutMs(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_type *err = NULL;
    ast_annotation *ta = tr_test_annotation(runner_state(self), args, "getTestTimeoutMs", &err);
    if (ta == NULL) return err;
    const char *tm = ast_annotation_find_value(ta, "timeoutMs");
    int64_t ms = (tm != NULL) ? strtoll(tm, NULL, 10) : 0;
    return &av_int_new(ms)->base.base;
}

/* The short Java class name of a value (AussomString, AussomList, ...),
 * for the ClassCastException messages below. */
static const char *tag_class(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_STRING:   return "AussomString";
    case C_TYPE_NULL:     return "AussomNull";
    case C_TYPE_INT:      return "AussomInt";
    case C_TYPE_DOUBLE:   return "AussomDouble";
    case C_TYPE_BOOL:     return "AussomBool";
    case C_TYPE_LIST:     return "AussomList";
    case C_TYPE_MAP:      return "AussomMap";
    case C_TYPE_CALLBACK: return "AussomCallback";
    default:              return "AussomObject";
    }
}

/* setIncludeTags / setExcludeTags keep their own copy of the tags, as
 * Java copies them into a set, so a later change to the script's list
 * does not change the filter. */
/* The tag set, without repeats. A tag that is not a string is the
 * ClassCastException ATestRunner's cast throws: *bad is set to its
 * Java class and NULL is returned. */
static av_list *copy_tags(av_list *tags, const char **bad)
{
    av_list *out = av_list_new();
    for (size_t i = 0; i < av_list_size(tags); i++) {
        av_type *v = av_list_get(tags, i);
        if (v->type != C_TYPE_STRING) {
            *bad = tag_class(v);
            return NULL;
        }
        if (list_has(out, av_string_get((av_string *)v))) continue;
        av_list_add(out, v);
    }
    return out;
}

static av_type *tags_cast_error(const char *bad, const char *to)
{
    char m[256];
    snprintf(m, sizeof(m), "java.lang.ClassCastException: class com.aussom.types.%s "
             "cannot be cast to class com.aussom.types.%s", bad, to);
    return &av_exception_new_thrown(m)->base.base;
}

/* setIncludeTags / setExcludeTags, in ATestRunner's order: the
 * (AussomList) cast of the argument, the (AussomString) cast of each
 * tag, then the runner call. The argument is checked before it is read
 * as a list; reading another value as one would use its memory as a
 * list's. */
static av_type *tr_set_tags(av_type *self, av_list *args, bool include)
{
    av_type *arg = av_list_get(args, 0);
    if (arg == NULL) {
        return &av_exception_new_thrown("java.lang.IndexOutOfBoundsException: "
            "Index 0 out of bounds for length 0")->base.base;
    }
    if (arg->type != C_TYPE_LIST) return tags_cast_error(tag_class(arg), "AussomList");
    const char *bad = NULL;
    av_list *tags = copy_tags((av_list *)arg, &bad);
    if (tags == NULL) return tags_cast_error(bad, "AussomString");
    av_type *denied = tr_no_runner(runner_state(self), include
        ? "setIncludeTags(java.util.Set)" : "setExcludeTags(java.util.Set)");
    if (denied != NULL) return denied;
    if (include) runner_state(self)->inc = tags;
    else runner_state(self)->exc = tags;
    return self;
}

static av_type *tr_setIncludeTags(av_type *self, environment *env, av_list *args)
{ (void)env; return tr_set_tags(self, args, true); }

static av_type *tr_setExcludeTags(av_type *self, environment *env, av_list *args)
{ (void)env; return tr_set_tags(self, args, false); }

static av_type *tr_shouldRun(av_type *self, environment *env, av_list *args)
{
    (void)env;
    trunner *t = runner_state(self);
    av_type *err = NULL;
    ast_annotation *ta = tr_test_annotation(t, args, "shouldRun", &err);
    if (ta == NULL) return err;
    return &av_bool_new(tags_should_run(t->inc, t->exc, parse_tags(ta)))->base.base;
}

static av_type *tr_runClassTests(av_type *self, environment *env, av_list *args)
{
    trunner *t = runner_state(self);
    const char *name = av_list_get_string(args, 0);
    av_type *denied = tr_no_runner(t, TR_BY_NAME);
    if (denied != NULL) return denied;
    ast_class *cls = tr_test_class(t, name);
    if (cls == NULL) return tr_class_not_found("runClassTests", name);
    av_type *thrown = NULL;
    test_result r = run_class_core(t->eng, cls, tr_test_tables(t, name), t->inc, t->exc, false, &thrown);
    /* A throw from @Before or @After escapes runClass (runFunction's
     * aussomException). */
    if (thrown != NULL) return tr_uncaught(env, "runClassTests", exmsg(thrown));
    av_map *m = av_map_new();
    av_map_put(m, "total", &av_int_new(r.total)->base.base);
    av_map_put(m, "skipped", &av_int_new(r.skipped)->base.base);
    av_map_put(m, "passed", &av_int_new(r.passed)->base.base);
    av_map_put(m, "failed", &av_int_new(r.failed)->base.base);
    return &m->base.base;
}

/* ATestRunner.getClassObject: the cached object for `cls`, else a new
 * one built in the runner's environment. */
static av_type *get_class_object(trunner *t, ast_class *cls)
{
    av_type *cached = av_strmap_get(t->class_objects, cls->base.name);
    if (cached != NULL) return cached;
    return class_instantiate(t->tenv, cls, false, av_list_new());
}

/* UnitTestRunner.runFunction: calls `fname` with no arguments on the
 * environment's current object. NULL on success, else the exception
 * the function raised. */
static av_type *run_function(trunner *t, ast_class *cls, const char *fname)
{
    av_type *ret = class_call(t->tenv, false, cls, fname, av_list_new());
    if (!av_is_ex(ret)) return NULL;
    /* runFunction throws new aussomException(ex.stackTraceToString()),
     * which escapes the extern method. */
    return &av_exception_new_thrown_aussom(exmsg(ret))->base.base;
}

/* runBefore and runAfter. `before` picks the hook, and whether the new
 * object always becomes the current one (runBefore) or only when there
 * is none yet (runAfter). The messages, including their method names
 * and spelling, are Java's. */
static av_type *tr_run_hook(av_type *self, av_list *args, bool before)
{
    trunner *t = runner_state(self);
    const char *name = av_list_get_string(args, 0);
    const char *has_name = before ? "hasBefore" : "hasAfter";
    av_type *denied = tr_no_runner(t, TR_BY_NAME);
    if (denied != NULL) return denied;
    ast_class *cls = tr_test_class(t, name);
    if (cls == NULL) return tr_class_not_found(has_name, name);
    const char *hook = hook_func(tr_test_tables(t, name), before ? "Before" : "After");
    if (hook == NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 before ? "testRunner.hasBefore(): Provided class '%s' has no @Before function set."
                        : "testRunner.hasAfter(): Provided class '%s' has no @After function set.",
                 name);
        return tr_exc(msg);
    }
    av_type *tci = get_class_object(t, cls);
    if (av_is_ex(tci)) return tci;
    if (before || environment_cur_obj(t->tenv) == NULL) {
        environment_set_cur_obj(t->tenv, tci);
    }
    av_type *err = run_function(t, cls, hook);
    return (err != NULL) ? err : self;
}

static av_type *tr_runBefore(av_type *self, environment *env, av_list *args)
{ (void)env; return tr_run_hook(self, args, true); }

static av_type *tr_runAfter(av_type *self, environment *env, av_list *args)
{ (void)env; return tr_run_hook(self, args, false); }

/* runTest(className, functName): runs one @Test function on the
 * current object (made from the class if there is none yet). A test
 * that throws answers with its exception. */
static av_type *tr_runTest(av_type *self, environment *env, av_list *args)
{
    (void)env;
    trunner *t = runner_state(self);
    const char *name = av_list_get_string(args, 0);
    const char *fn = av_list_get_string(args, 1);
    av_type *denied = tr_no_runner(t, TR_BY_NAME);
    if (denied != NULL) return denied;
    ast_class *cls = tr_test_class(t, name);
    if (cls == NULL) return tr_class_not_found("runTest", name);
    av_type *tci = get_class_object(t, cls);
    if (av_is_ex(tci)) return tci;
    if (environment_cur_obj(t->tenv) == NULL) environment_set_cur_obj(t->tenv, tci);

    char msg[512];
    ast_funct_def *fd = (fn != NULL) ? find_func(cls, fn) : NULL;
    if (fd == NULL) {
        snprintf(msg, sizeof(msg),
                 "testRunner.runTest(): Provided class '%s' has no function '%s'.",
                 name, fn != NULL ? fn : "");
        return tr_exc(msg);
    }
    if (func_annotation(fd, "Test") == NULL) {
        snprintf(msg, sizeof(msg),
                 "testRunner.runTest(): Provided class '%s' function '%s' has no @Test annotation.",
                 name, fn);
        return tr_exc(msg);
    }
    av_type *err = run_function(t, cls, fn);
    return (err != NULL) ? err : self;
}

static av_type *tr_clearClassObjectCache(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    runner_state(self)->class_objects = av_strmap_new();
    return self;
}

/* --- test extern class (ATest) ------------------------------------ */

/* test.runTestsForClass(ClassName): runs one class's tests in a fresh
 * engine built with the test security manager (TestSecurityManagerImpl,
 * runner permission left off), parsing the file the class came from.
 * Gated on test.aussom.runner as read from the running engine. Returns
 * the {total, skipped, passed, failed} map. Ports
 * ATest.runTestsForClass. */
static av_type *t_runTestsForClass(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *test_class_name = av_list_get_string(args, 0);
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng),
                                               "test.aussom.runner", false)) {
        return tr_exc("testRunner.runTestsForClass(): Security exception, "
                      "action 'test.aussom.runner' not permitted.");
    }

    char msg[512];
    ast_class *lcls = (test_class_name != NULL)
        ? engine_get_class_by_name(eng, test_class_name) : NULL;
    if (lcls == NULL) {
        snprintf(msg, sizeof(msg),
                 "test.runTestsForClass(): Provided class '%s' not found.",
                 test_class_name != NULL ? test_class_name : "");
        return tr_exc(msg);
    }
    const char *script_file = lcls->base.file_name;

    engine *runner = engine_new_with_security(test_security_manager_impl_new());
    /* Engine.parseFile: an error Java throws escapes this method; a
     * syntax error is logged and the classes read before it load. */
    if (script_file == NULL) script_file = "";
    aussom_parse_file(runner, script_file);
    const char *parse_thrown = engine_parse_exception(runner);
    if (parse_thrown != NULL)
        return tr_uncaught(env, "runTestsForClass", parse_thrown);

    /* loadTestClasses: the file must hold at least one class. */
    bool found = false;
    size_t n = engine_class_count(runner);
    for (size_t i = 0; i < n && !found; i++) {
        const char *fn = engine_get_class(runner, i)->base.file_name;
        found = fn != NULL && strcmp(fn, script_file) == 0;
    }
    if (!found) {
        return tr_uncaught(env, "runTestsForClass",
            "Engine.runTest(): No classes found for that script file.");
    }

    /* getTestClass gives a class with no tests an empty test list, so
     * it runs nothing and reports zeros. */
    ast_class *cls = engine_get_class_by_name(runner, test_class_name);
    if (cls == NULL) {
        return &av_exception_new_thrown("java.lang.NullPointerException: Cannot invoke "
            "\"com.aussom.ast.astClass.getTestClass()\" because \"cls\" is null")->base.base;
    }

    /* ATest logs the exception and throws it on. */
    av_type *thrown = NULL;
    test_result r = run_class_core(runner, cls, NULL, NULL, NULL, false, &thrown);
    if (thrown != NULL) {
        engine_err(runner, exmsg(thrown));
        return tr_uncaught(env, "runTestsForClass", exmsg(thrown));
    }
    av_map *m = av_map_new();
    av_map_put(m, "total", &av_int_new(r.total)->base.base);
    av_map_put(m, "skipped", &av_int_new(r.skipped)->base.base);
    av_map_put(m, "passed", &av_int_new(r.passed)->base.base);
    av_map_put(m, "failed", &av_int_new(r.failed)->base.base);
    return &m->base.base;
}

const aussom_extern_method stdlib_atest_methods[] = {
    { "runTestsForClass", t_runTestsForClass },
    { NULL, NULL }
};

const aussom_extern_method stdlib_atest_runner_methods[] = {
    { "newTestRunner", tr_newTestRunner },
    { "loadTestString", tr_loadTestString },
    { "loadTestFile", tr_loadTestFile },
    { "getTestClasses", tr_getTestClasses },
    { "getTestFunctions", tr_getTestFunctions },
    { "hasBefore", tr_hasBefore },
    { "hasAfter", tr_hasAfter },
    { "hasBeforeEach", tr_hasBeforeEach },
    { "hasAfterEach", tr_hasAfterEach },
    { "hasOnTestFail", tr_hasOnTestFail },
    { "getTestTags", tr_getTestTags },
    { "getTestTimeoutMs", tr_getTestTimeoutMs },
    { "setIncludeTags", tr_setIncludeTags },
    { "setExcludeTags", tr_setExcludeTags },
    { "shouldRun", tr_shouldRun },
    { "runClassTests", tr_runClassTests },
    { "runBefore", tr_runBefore },
    { "runAfter", tr_runAfter },
    { "runTest", tr_runTest },
    { "clearClassObjectCache", tr_clearClassObjectCache },
    { NULL, NULL }
};
