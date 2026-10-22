/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for the debugging interface: engine_set_debugger and the hooks
 * in ast_node_eval, the breakpoint calls, engine_eval_in_frame, call
 * stack frame locals, engine_thread_id and the main(args) list. Ports
 * aussom-base's src/test/java/Debugger.java, numbered the same way,
 * plus cases for what is C-specific: a hook returning false to stop the
 * run, where Java's throws.
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aussom/aussom_gc.h"
#include "aussom/ast.h"
#include "aussom/av.h"
#include "aussom/callstack.h"
#include "aussom/console.h"
#include "aussom/debugger.h"
#include "aussom/engine.h"
#include "aussom/environment.h"
#include "aussom/parser.h"
#include "aussom/security_manager.h"
#include "greatest.h"

/* Programs that fail on purpose log through their engine. Keep the last
 * error so a case can check what the run stopped with. */
static char last_err[4096];
static void quiet(const char *s) { (void)s; }
static void keep_err(const char *s)
{
    snprintf(last_err, sizeof(last_err), "%s", s);
}
static const console_logger silent = {
    quiet, quiet, quiet, quiet, quiet, keep_err, quiet, quiet,
};

/* Engine(new TestSecurityManagerImpl()), which permits the debugger. */
static engine *new_engine(void)
{
    engine *e = engine_new_with_security(test_security_manager_impl_new());
    engine_set_logger(e, &silent);
    last_err[0] = '\0';
    return e;
}

static engine *engine_with(const char *src)
{
    engine *e = new_engine();
    if (!aussom_parse_string(e, src, "test.aus")) return NULL;
    return e;
}

#define MAIN_SRC(body) "class App { public main(args) { " body " } }"

/* --- the recording debugger -------------------------------------- */

/* CapturingDebugger, FrameCapturingDebugger and
 * PauseFromOutsideDebugger from the Java suite in one. Static, so the
 * collector sees the pointers it holds. */
#define MAX_EVENTS 4096

typedef struct pause_event {
    int64_t tid;
    ast_node *node;
    aussom_pause_reason reason;
    callstack *top;
    bool released;
} pause_event;

typedef struct recorder {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pause_event events[MAX_EVENTS];
    size_t nevents;
    av_exception *exceptions[64];
    size_t nexceptions;

    /* Behaviour. */
    bool block_on_pause;
    volatile bool step_once;
    volatile bool step_enabled;
    volatile int64_t pause_pending_tid;
    bool stop_on_pause;
    bool stop_on_exception;

    /* engine_eval_in_frame from inside the first pause. */
    engine *eng;
    const char *eval_src;
    av_type *eval_result;

    /* Frame names seen in any pause's call stack chain. */
    const char *want_frame;      /* a substring of a function name */
    bool saw_frame;
    bool frame_had_called_function;
    const char *want_called;     /* a called_function's name */
    bool saw_called;
} recorder;

static recorder R;

static void recorder_reset(void)
{
    pthread_mutex_destroy(&R.lock);
    pthread_cond_destroy(&R.cond);
    memset(&R, 0, sizeof(R));
    pthread_mutex_init(&R.lock, NULL);
    pthread_cond_init(&R.cond, NULL);
}

/* Caller holds R.lock. */
static void note_chain(callstack *top)
{
    for (callstack *cs = top; cs != NULL; cs = cs->parent) {
        if (R.want_frame != NULL && cs->function_name != NULL
            && strstr(cs->function_name, R.want_frame) != NULL) {
            R.saw_frame = true;
            if (cs->called_function != NULL) R.frame_had_called_function = true;
        }
        if (R.want_called != NULL && cs->called_function != NULL
            && strcmp(cs->called_function->base.name, R.want_called) == 0) {
            R.saw_called = true;
        }
    }
}

static bool rec_on_pause(void *data, ast_node *node, environment *env,
                         aussom_pause_reason reason)
{
    (void)data;
    int64_t tid = engine_thread_id();

    if (R.eval_src != NULL && R.eval_result == NULL) {
        R.eval_result = engine_eval_in_frame(R.eng, R.eval_src, env);
    }

    pthread_mutex_lock(&R.lock);
    size_t idx = R.nevents++;
    if (idx < MAX_EVENTS) {
        R.events[idx].tid = tid;
        R.events[idx].node = node;
        R.events[idx].reason = reason;
        R.events[idx].top = env->st;
        R.events[idx].released = false;
    }
    note_chain(env->st);
    pthread_cond_broadcast(&R.cond);
    if (R.block_on_pause && idx < MAX_EVENTS) {
        while (!R.events[idx].released) pthread_cond_wait(&R.cond, &R.lock);
    }
    bool go = !R.stop_on_pause;
    pthread_mutex_unlock(&R.lock);
    return go;
}

static bool rec_should_pause_for_step(void *data, ast_node *node, environment *env)
{
    (void)data; (void)node; (void)env;
    if (R.step_enabled) return true;
    if (R.step_once) { R.step_once = false; return true; }
    if (R.pause_pending_tid != 0 && R.pause_pending_tid == engine_thread_id()) {
        R.pause_pending_tid = 0;
        return true;
    }
    return false;
}

static bool rec_on_exception(void *data, av_exception *ex, environment *env)
{
    (void)data; (void)env;
    pthread_mutex_lock(&R.lock);
    if (R.nexceptions < 64) R.exceptions[R.nexceptions] = ex;
    R.nexceptions++;
    bool go = !R.stop_on_exception;
    pthread_mutex_unlock(&R.lock);
    return go;
}

static const aussom_debugger rec_debugger = {
    NULL, rec_on_pause, rec_should_pause_for_step, rec_on_exception,
};

static size_t count_reason(aussom_pause_reason reason)
{
    size_t n = 0;
    for (size_t i = 0; i < R.nevents && i < MAX_EVENTS; i++) {
        if (R.events[i].reason == reason) n++;
    }
    return n;
}

/* Waits up to `ms` for at least `n` pauses. */
static bool await_pause_count(size_t n, int ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&R.lock);
    while (R.nevents < n) {
        if (pthread_cond_timedwait(&R.cond, &R.lock, &until) != 0) break;
    }
    bool ok = R.nevents >= n;
    pthread_mutex_unlock(&R.lock);
    return ok;
}

static void release_thread(int64_t tid)
{
    pthread_mutex_lock(&R.lock);
    for (size_t i = 0; i < R.nevents && i < MAX_EVENTS; i++) {
        if (R.events[i].tid == tid) R.events[i].released = true;
    }
    pthread_cond_broadcast(&R.cond);
    pthread_mutex_unlock(&R.lock);
}

/* The first node on the line of type `t`. */
static ast_node *pick_node_on_line(engine *e, int line, ast_node_type t)
{
    ast_node *hits[64];
    size_t n = engine_find_nodes_by_line(e, "test.aus", line, hits, 64);
    for (size_t i = 0; i < n && i < 64; i++) {
        if (hits[i]->type == t) return hits[i];
    }
    return NULL;
}

/* An environment with no `this`, empty locals and an empty call stack,
 * as the Java evalInFrame tests build. */
static environment *bare_env(engine *e)
{
    environment *env = environment_new(e);
    environment_set(env, NULL, av_strmap_new_unlocked(), callstack_new());
    return env;
}

/* A thread running engine_run. */
typedef struct run_job {
    engine *eng;
    int rc;
    volatile int64_t tid;
} run_job;

static void *run_worker(void *arg)
{
    run_job *job = arg;
    job->tid = engine_thread_id();
    job->rc = engine_run(job->eng, "<test>");
    return NULL;
}

/* --- 1, 2: registration and gating ------------------------------- */

TEST default_off(void)
{
    engine *e = new_engine();
    ASSERT_FALSE(engine_is_debug_mode(e));
    ASSERT_EQ(NULL, engine_get_debugger(e));
    PASS();
}

TEST set_debugger_denied_under_default_security_manager(void)
{
    recorder_reset();
    engine *e = engine_new_with_security(default_security_manager_impl_new());
    engine_set_logger(e, &silent);
    const char *err = engine_set_debugger(e, &rec_debugger);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "aussom.debugger.enable") != NULL);
    ASSERT_FALSE(engine_is_debug_mode(e));
    ASSERT_EQ(NULL, engine_get_debugger(e));
    PASS();
}

TEST clearing_the_debugger_is_always_allowed(void)
{
    engine *e = engine_new_with_security(default_security_manager_impl_new());
    engine_set_logger(e, &silent);
    ASSERT_EQ(NULL, engine_set_debugger(e, NULL));
    ASSERT_FALSE(engine_is_debug_mode(e));
    PASS();
}

TEST set_debugger_registers_and_clears(void)
{
    recorder_reset();
    engine *e = new_engine();
    ASSERT_EQ(NULL, engine_set_debugger(e, &rec_debugger));
    ASSERT(engine_is_debug_mode(e));
    ASSERT_EQ(&rec_debugger, engine_get_debugger(e));
    ASSERT_EQ(NULL, engine_set_debugger(e, NULL));
    ASSERT_FALSE(engine_is_debug_mode(e));
    ASSERT_EQ(NULL, engine_get_debugger(e));
    PASS();
}

SUITE(registration)
{
    RUN_TEST(default_off);
    RUN_TEST(set_debugger_denied_under_default_security_manager);
    RUN_TEST(clearing_the_debugger_is_always_allowed);
    RUN_TEST(set_debugger_registers_and_clears);
}

/* --- 3, 4, 5, 8: breakpoint and step hooks ----------------------- */

TEST breakpoint_triggers_on_pause(void)
{
    recorder_reset();
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 5;\n"
                            "} }");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    ast_node *hits[16];
    size_t n = engine_find_nodes_by_line(e, "test.aus", 2, hits, 16);
    ASSERT(n > 0);
    hits[0]->breakpoint = true;
    engine_run(e, "<test>");
    ASSERT(R.nevents >= 1);
    ast_node *first = NULL;
    for (size_t i = 0; i < R.nevents; i++) {
        if (R.events[i].reason == AUSSOM_PAUSE_BREAKPOINT) { first = R.events[i].node; break; }
    }
    ASSERT(first != NULL);
    ASSERT_EQ(hits[0], first);
    PASS();
}

TEST no_breakpoint_no_pause(void)
{
    recorder_reset();
    engine *e = engine_with(MAIN_SRC("x = 5; y = 7;"));
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    engine_run(e, "<test>");
    ASSERT_EQ(0, R.nevents);
    PASS();
}

TEST should_pause_for_step_gates_step(void)
{
    recorder_reset();
    engine *e = engine_with(MAIN_SRC("x = 5; y = 7;"));
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    R.step_once = true;
    engine_run(e, "<test>");
    ASSERT_EQ(1, count_reason(AUSSOM_PAUSE_STEP));
    PASS();
}

TEST run_unchanged_with_debug_on(void)
{
    recorder_reset();
    engine *e = engine_with(MAIN_SRC("x = 5; return 42;"));
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    ASSERT_EQ(42, engine_run(e, "<test>"));
    ASSERT_EQ(0, R.nevents);
    ASSERT_EQ(0, R.nexceptions);
    PASS();
}

SUITE(hooks)
{
    RUN_TEST(breakpoint_triggers_on_pause);
    RUN_TEST(no_breakpoint_no_pause);
    RUN_TEST(should_pause_for_step_gates_step);
    RUN_TEST(run_unchanged_with_debug_on);
}

/* --- C only: a hook returning false stops the run ---------------- */

TEST on_pause_returning_false_stops_the_run(void)
{
    recorder_reset();
    /* The try/catch must not swallow the stop. */
    engine *e = engine_with("class App { public main(args) {\n"
                            "    try {\n"
                            "        x = 5;\n"
                            "    } catch (ex) { return 9; }\n"
                            "    return 3;\n"
                            "} }");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    ASSERT(engine_set_breakpoint(e, "test.aus", 3));
    R.stop_on_pause = true;
    ASSERT_EQ(1, engine_run(e, "<test>"));
    ASSERT(strstr(last_err, ENGINE_CANCELLED_EXCEPTION_ID) != NULL);
    PASS();
}

TEST on_exception_returning_false_stops_the_run(void)
{
    recorder_reset();
    engine *e = engine_with("class App { public main(args) {\n"
                            "    try {\n"
                            "        throw \"boom\";\n"
                            "    } catch (ex) { return 9; }\n"
                            "    return 3;\n"
                            "} }");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    R.stop_on_exception = true;
    ASSERT_EQ(1, engine_run(e, "<test>"));
    ASSERT(strstr(last_err, ENGINE_CANCELLED_EXCEPTION_ID) != NULL);
    PASS();
}

SUITE(stopping)
{
    RUN_TEST(on_pause_returning_false_stops_the_run);
    RUN_TEST(on_exception_returning_false_stops_the_run);
}

/* --- 6, 17: threads ---------------------------------------------- */

TEST two_threads_pause_independently(void)
{
    recorder_reset();
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 5;\n"
                            "} }");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    R.block_on_pause = true;
    ASSERT(engine_set_breakpoint(e, "test.aus", 2));

    run_job j1 = { e, -1, 0 }, j2 = { e, -1, 0 };
    pthread_t t1, t2;
    ASSERT_EQ(0, pthread_create(&t1, NULL, run_worker, &j1));
    ASSERT_EQ(0, pthread_create(&t2, NULL, run_worker, &j2));

    ASSERT(await_pause_count(2, 5000));
    ASSERT(R.events[0].tid != R.events[1].tid);

    release_thread(R.events[0].tid);
    release_thread(R.events[1].tid);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    ASSERT_EQ(0, j1.rc);
    ASSERT_EQ(0, j2.rc);
    PASS();
}

TEST pause_from_outside(void)
{
    recorder_reset();
    /* Long enough that the request lands while the loop is running. */
    engine *e = engine_with(MAIN_SRC(
        "for (i = 0; i < 1000000; i = i + 1) { x = i; }"));
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    R.block_on_pause = true;

    run_job job = { e, -1, 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, run_worker, &job));
    while (job.tid == 0) sched_yield();

    R.pause_pending_tid = job.tid;
    ASSERT(await_pause_count(1, 5000));
    ASSERT_EQ(AUSSOM_PAUSE_STEP, R.events[0].reason);
    ASSERT_EQ(job.tid, R.events[0].tid);

    release_thread(job.tid);
    pthread_join(t, NULL);
    ASSERT_EQ(0, job.rc);
    PASS();
}

TEST thread_ids_are_stable_and_distinct(void)
{
    int64_t mine = engine_thread_id();
    ASSERT(mine > 0);
    ASSERT_EQ(mine, engine_thread_id());

    engine *e = engine_with(MAIN_SRC("return 0;"));
    ASSERT(e != NULL);
    run_job job = { e, -1, 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, run_worker, &job));
    pthread_join(t, NULL);
    ASSERT(job.tid > 0);
    ASSERT(job.tid != mine);
    PASS();
}

SUITE(threads)
{
    RUN_TEST(two_threads_pause_independently);
    RUN_TEST(pause_from_outside);
    RUN_TEST(thread_ids_are_stable_and_distinct);
}

/* --- 7: find_nodes_by_line and the breakpoint calls -------------- */

TEST find_nodes_returns_nodes_for_a_known_line(void)
{
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 5;\n"
                            "    y = x + 1;\n"
                            "} }");
    ASSERT(e != NULL);
    ASSERT(engine_find_nodes_by_line(e, "test.aus", 2, NULL, 0) > 0);
    ASSERT(engine_find_nodes_by_line(e, "test.aus", 3, NULL, 0) > 0);
    ASSERT_EQ(0, engine_find_nodes_by_line(e, "test.aus", 99, NULL, 0));
    ASSERT_EQ(0, engine_find_nodes_by_line(e, "other.aus", 2, NULL, 0));
    PASS();
}

TEST find_nodes_on_a_multi_expression_line(void)
{
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 1 + 2 + 3;\n"
                            "} }");
    ASSERT(e != NULL);
    ASSERT(engine_find_nodes_by_line(e, "test.aus", 2, NULL, 0) >= 2);
    PASS();
}

TEST find_nodes_writes_no_more_than_max(void)
{
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 1 + 2 + 3;\n"
                            "} }");
    ASSERT(e != NULL);
    ast_node *one[2] = { NULL, NULL };
    size_t total = engine_find_nodes_by_line(e, "test.aus", 2, one, 1);
    ASSERT(total >= 2);
    ASSERT(one[0] != NULL);
    ASSERT_EQ(NULL, one[1]);
    PASS();
}

TEST set_and_clear_breakpoints(void)
{
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 5;\n"
                            "    y = x + 1;\n"
                            "} }");
    ASSERT(e != NULL);
    ASSERT_FALSE(engine_set_breakpoint(e, "test.aus", 99));
    ASSERT(engine_set_breakpoint(e, "test.aus", 2));

    /* Only the first node on the line is marked. */
    ast_node *hits[16];
    size_t n = engine_find_nodes_by_line(e, "test.aus", 2, hits, 16);
    ASSERT(hits[0]->breakpoint);
    for (size_t i = 1; i < n; i++) ASSERT_FALSE(hits[i]->breakpoint);

    ASSERT(engine_clear_breakpoint(e, "test.aus", 2));
    ASSERT_FALSE(engine_clear_breakpoint(e, "test.aus", 2));
    ASSERT_FALSE(hits[0]->breakpoint);

    ASSERT(engine_set_breakpoint(e, "test.aus", 2));
    ASSERT(engine_set_breakpoint(e, "test.aus", 3));
    ASSERT(engine_clear_all_breakpoints(e));
    ASSERT_FALSE(engine_clear_all_breakpoints(e));
    PASS();
}

SUITE(find_nodes)
{
    RUN_TEST(find_nodes_returns_nodes_for_a_known_line);
    RUN_TEST(find_nodes_on_a_multi_expression_line);
    RUN_TEST(find_nodes_writes_no_more_than_max);
    RUN_TEST(set_and_clear_breakpoints);
}

/* --- 9, 11: the exception hook ----------------------------------- */

TEST on_exception_fires_once_per_value(void)
{
    recorder_reset();
    engine *e = engine_with("class App {\n"
                            "  public deep() { throw \"boom\"; }\n"
                            "  public mid() { return this.deep(); }\n"
                            "  public main(args) { x = this.mid(); return 0; }\n"
                            "}");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    engine_run(e, "<test>");
    ASSERT_EQ(1, R.nexceptions);
    ASSERT(av_exception_is_debugger_seen(R.exceptions[0]));
    PASS();
}

TEST runtime_error_fires_once_across_frames(void)
{
    recorder_reset();
    engine *e = engine_with("class App {\n"
                            "  public deep() { return new NoSuchClass(); }\n"
                            "  public mid() { return this.deep(); }\n"
                            "  public main(args) { x = this.mid(); return 0; }\n"
                            "}");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    engine_run(e, "<test>");
    ASSERT_EQ(1, R.nexceptions);
    PASS();
}

SUITE(exceptions)
{
    RUN_TEST(on_exception_fires_once_per_value);
    RUN_TEST(runtime_error_fires_once_across_frames);
}

/* --- 14 to 17: eval_in_frame ------------------------------------- */

TEST eval_in_frame_resolves_locals(void)
{
    recorder_reset();
    engine *e = engine_with("class App { public main(args) {\n"
                            "    x = 41;\n"
                            "    y = 0;\n"
                            "} }");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    ASSERT(engine_set_breakpoint(e, "test.aus", 3));
    R.eng = e;
    R.eval_src = "x + 1;";
    ASSERT_EQ(0, engine_run(e, "<test>"));
    ASSERT(R.eval_result != NULL);
    ASSERT_EQ(C_TYPE_INT, R.eval_result->type);
    ASSERT_EQ(42, av_int_get((av_int *)R.eval_result));
    PASS();
}

TEST eval_in_frame_sees_this(void)
{
    recorder_reset();
    engine *e = engine_with("class App {\n"
                            "  public v = 6;\n"
                            "  public main(args) {\n"
                            "    y = 0;\n"
                            "  }\n"
                            "}");
    ASSERT(e != NULL);
    engine_set_debugger(e, &rec_debugger);
    ASSERT(engine_set_breakpoint(e, "test.aus", 4));
    R.eng = e;
    R.eval_src = "this.v * 7;";
    ASSERT_EQ(0, engine_run(e, "<test>"));
    ASSERT(R.eval_result != NULL);
    ASSERT_EQ(C_TYPE_INT, R.eval_result->type);
    ASSERT_EQ(42, av_int_get((av_int *)R.eval_result));
    PASS();
}

TEST eval_in_frame_parse_error(void)
{
    engine *e = new_engine();
    av_type *ret = engine_eval_in_frame(e, "x = ;", bare_env(e));
    ASSERT(av_is_ex(ret));
    ASSERT_STR_EQ("Engine.evalInFrame: parse error.",
                  av_exception_get_text((av_exception *)ret));
    ASSERT_FALSE(engine_has_parse_error(e));
    PASS();
}

TEST eval_in_frame_runtime_error_is_a_value(void)
{
    engine *e = new_engine();
    av_type *ret = engine_eval_in_frame(e, "1/0;", bare_env(e));
    ASSERT(av_is_ex(ret));
    PASS();
}

TEST eval_in_frame_denied_under_default_security_manager(void)
{
    engine *e = engine_new_with_security(default_security_manager_impl_new());
    engine_set_logger(e, &silent);
    av_type *ret = engine_eval_in_frame(e, "1 + 1;", bare_env(e));
    ASSERT(av_is_ex(ret));
    ASSERT(strstr(av_exception_get_text((av_exception *)ret),
                  "aussom.debugger.enable") != NULL);
    PASS();
}

TEST eval_in_frame_returns_the_last_value(void)
{
    engine *e = new_engine();
    environment *env = bare_env(e);
    av_type *ret = engine_eval_in_frame(e, "a = 2; a * 3;", env);
    ASSERT_EQ(C_TYPE_INT, ret->type);
    ASSERT_EQ(6, av_int_get((av_int *)ret));

    ret = engine_eval_in_frame(e, "return 5; 7;", env);
    ASSERT_EQ(C_TYPE_INT, ret->type);
    ASSERT_EQ(5, av_int_get((av_int *)ret));

    /* A class declaration is parsed into the engine and leaves no
     * statement to run. (Empty source is a parse error, in Java too.) */
    ret = engine_eval_in_frame(e, "class EvalOnly { }", env);
    ASSERT_EQ(C_TYPE_NULL, ret->type);
    PASS();
}

SUITE(eval_in_frame)
{
    RUN_TEST(eval_in_frame_resolves_locals);
    RUN_TEST(eval_in_frame_sees_this);
    RUN_TEST(eval_in_frame_parse_error);
    RUN_TEST(eval_in_frame_runtime_error_is_a_value);
    RUN_TEST(eval_in_frame_denied_under_default_security_manager);
    RUN_TEST(eval_in_frame_returns_the_last_value);
}

/* --- synthetic call stack frames --------------------------------- */

TEST member_init_frame(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    ASSERT(aussom_parse_string(e,
        "class Box { public x = 1; }\n"
        "class App { public main(args) {\n"
        "    b = new Box();\n"
        "} }", "test.aus"));
    ast_node *bp = pick_node_on_line(e, 1, AST_TYPE_INT);
    ASSERT(bp != NULL);
    bp->breakpoint = true;
    R.want_frame = "<member-init>";
    engine_run(e, "<test>");
    ASSERT(R.saw_frame);
    /* Class-scoped, so it names no function. */
    ASSERT_FALSE(R.frame_had_called_function);
    PASS();
}

TEST inherited_member_init_frame(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    ASSERT(aussom_parse_string(e,
        "class Base { public p = 1; }\n"
        "class Derived : Base { public q = 2; }\n"
        "class App { public main(args) {\n"
        "    obj = new Derived();\n"
        "} }", "test.aus"));
    ast_node *bp = pick_node_on_line(e, 1, AST_TYPE_INT);
    ASSERT(bp != NULL);
    bp->breakpoint = true;
    R.want_frame = "<member-init>";
    engine_run(e, "<test>");
    ASSERT(R.saw_frame);
    PASS();
}

TEST arg_defaults_frame(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    ASSERT(aussom_parse_string(e,
        "class App {\n"
        "    public foo(int x = 42) { return x; }\n"
        "    public main(args) {\n"
        "        a = this.foo();\n"
        "    }\n"
        "}", "test.aus"));
    R.want_frame = "<arg-defaults>";
    R.want_called = "foo";
    R.step_enabled = true;
    engine_run(e, "<test>");
    R.step_enabled = false;
    ASSERT(R.saw_frame);
    ASSERT(R.saw_called);
    PASS();
}

TEST static_init_frame(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    R.want_frame = "<static-init>";
    R.step_enabled = true;
    aussom_parse_string(e, "static class S { public v = 7; }\n", "test.aus");
    R.step_enabled = false;
    ASSERT(R.saw_frame);
    PASS();
}

TEST reflect_get_methods_frame(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    ASSERT(aussom_parse_string(e,
        "include reflect;\n"
        "class Target {\n"
        "    public foo(int x = 42) { return x; }\n"
        "}\n"
        "class App {\n"
        "    public main(args) {\n"
        "        rc = reflect.getClassDef(\"Target\");\n"
        "        m = rc.getMethods();\n"
        "    }\n"
        "}", "test.aus"));
    ast_node *bp = pick_node_on_line(e, 3, AST_TYPE_INT);
    ASSERT(bp != NULL);
    bp->breakpoint = true;
    R.want_frame = "<reflect.getMethods>";
    engine_run(e, "<test>");
    ASSERT(R.saw_frame);
    PASS();
}

TEST defined_frame_has_called_function(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    ASSERT(aussom_parse_string(e,
        "class App {\n"
        "    public main(args) {\n"
        "        x = 1;\n"
        "    }\n"
        "}", "test.aus"));
    ast_node *bp = pick_node_on_line(e, 3, AST_TYPE_INT);
    ASSERT(bp != NULL);
    bp->breakpoint = true;
    engine_run(e, "<test>");
    ASSERT(R.nevents >= 1);
    callstack *top = R.events[0].top;
    ASSERT(top->called_function != NULL);
    ASSERT_STR_EQ("main", top->called_function->base.name);
    PASS();
}

/* CallStack.getLocals: a pause in a nested call can read the locals of
 * the frames below it. */
TEST frames_carry_their_locals(void)
{
    recorder_reset();
    engine *e = new_engine();
    engine_set_debugger(e, &rec_debugger);
    ASSERT(aussom_parse_string(e,
        "class App {\n"
        "    public inner(int a) {\n"
        "        y = a + 1;\n"
        "    }\n"
        "    public main(args) {\n"
        "        x = 41;\n"
        "        this.inner(x);\n"
        "    }\n"
        "}", "test.aus"));
    ASSERT(engine_set_breakpoint(e, "test.aus", 3));
    engine_run(e, "<test>");
    ASSERT(R.nevents >= 1);

    av_strmap *inner_locals = NULL, *main_locals = NULL;
    for (callstack *cs = R.events[0].top; cs != NULL; cs = cs->parent) {
        if (cs->called_function == NULL || cs->locals == NULL) continue;
        if (strcmp(cs->called_function->base.name, "inner") == 0) inner_locals = cs->locals;
        if (strcmp(cs->called_function->base.name, "main") == 0) main_locals = cs->locals;
    }
    ASSERT(inner_locals != NULL);
    ASSERT(main_locals != NULL);
    av_type *a = av_strmap_get(inner_locals, "a");
    av_type *x = av_strmap_get(main_locals, "x");
    ASSERT(a != NULL && a->type == C_TYPE_INT);
    ASSERT(x != NULL && x->type == C_TYPE_INT);
    ASSERT_EQ(41, av_int_get((av_int *)a));
    ASSERT_EQ(41, av_int_get((av_int *)x));
    PASS();
}

SUITE(frames)
{
    RUN_TEST(member_init_frame);
    RUN_TEST(inherited_member_init_frame);
    RUN_TEST(arg_defaults_frame);
    RUN_TEST(static_init_frame);
    RUN_TEST(reflect_get_methods_frame);
    RUN_TEST(defined_frame_has_called_function);
    RUN_TEST(frames_carry_their_locals);
}

/* --- main(args) -------------------------------------------------- */

TEST main_gets_no_args_by_default(void)
{
    engine *e = engine_with(MAIN_SRC("return args.size();"));
    ASSERT(e != NULL);
    ASSERT_EQ(0, engine_run(e, "<test>"));
    PASS();
}

TEST main_gets_the_args_added(void)
{
    engine *e = engine_with(MAIN_SRC(
        "if (args.size() == 3 && args[0] == \"one\" && args[1] == \"two\""
        " && args[2] == \"three\") { return 7; } return 1;"));
    ASSERT(e != NULL);
    engine_add_main_arg(e, "one");
    const char *rest[] = { "two", "three" };
    engine_add_main_args(e, rest, 2);
    ASSERT_EQ(7, engine_run(e, "<test>"));
    PASS();
}

SUITE(main_args)
{
    RUN_TEST(main_gets_no_args_by_default);
    RUN_TEST(main_gets_the_args_added);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    /* Must run on this thread before any engine exists. */
    aussom_gc_init();
    pthread_mutex_init(&R.lock, NULL);
    pthread_cond_init(&R.cond, NULL);

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(registration);
    RUN_SUITE(hooks);
    RUN_SUITE(stopping);
    RUN_SUITE(threads);
    RUN_SUITE(find_nodes);
    RUN_SUITE(exceptions);
    RUN_SUITE(eval_in_frame);
    RUN_SUITE(frames);
    RUN_SUITE(main_args);
    GREATEST_MAIN_END();
}
