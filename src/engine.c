/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/callstack.h"
#include "aussom/ast_eval.h"
#include "aussom/engine.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "aussom/av.h"
#include "aussom/av_list.h"
#include "aussom/av_strmap.h"
#include "aussom/ast_class.h"
#include "aussom/console.h"
#include "aussom/environment.h"
#include "aussom/aussom_gc.h"
#include "aussom/parser.h"
#include "aussom/security_manager.h"
#include "aussom/stdlib_packages.h"
#include "aussom/stdlib_sources.h"

static void seed_base_modules(engine *e);
static void parse_lang_source(engine *e);

engine *engine_new(void)
{
    return engine_new_with_security(security_manager_impl_new());
}

engine *engine_new_with_security(security_manager *secman)
{
    engine *e = AUSSOM_ALLOC(sizeof(*e));
    e->secman = secman;
    ast_node_array_init(&e->classes);
    e->logger = NULL;                 /* built-in default */
    e->log_level = CONSOLE_INFO;
    e->modules = NULL;
    e->modules_count = 0;
    e->modules_capacity = 0;
    seed_base_modules(e);
    e->extern_packages = NULL;
    e->extern_packages_count = 0;
    e->extern_packages_capacity = 0;
    pthread_mutex_init(&e->extern_packages_lock, NULL);
    engine_add_extern_package(e, &stdlib_types_package);
    engine_add_extern_package(e, &stdlib_stdlib_package);
    stdlib_install_native_str();
    for (size_t i = 0; i < sizeof(e->type_defs) / sizeof(e->type_defs[0]); i++) {
        e->type_defs[i] = NULL;
    }
    e->includes = NULL;
    e->includes_count = 0;
    e->includes_capacity = 0;
    e->include_paths = NULL;
    e->include_paths_count = 0;
    e->include_paths_capacity = 0;
    e->exclude_paths = NULL;
    e->exclude_paths_count = 0;
    e->exclude_paths_capacity = 0;
    e->parse_error = false;
    e->parse_exception = NULL;
    e->parse_exception_stack_trace = NULL;
    e->init_complete = false;
    e->static_classes = NULL;
    e->diags = NULL;
    e->diags_count = 0;
    e->diags_capacity = 0;
    e->script_env = NULL;
    e->control = CONTROL_RUNNING;
    pthread_mutex_init(&e->control_lock, NULL);
    pthread_cond_init(&e->control_cond, NULL);
    e->threads = NULL;
    e->threads_count = 0;
    e->threads_capacity = 0;
    e->banked_cpu_nanos = 0;
    e->banked_alloc_bytes = 0;
    /* Resource limits come from policy. Read once here and again at
     * the start of each program, so a host that rewrites policy between
     * runs is honored and the interpreter never pays for a property
     * lookup on the hot path. */
    e->limits.call_depth = ENGINE_DEFAULT_CALL_DEPTH;
    e->limits.regex_steps = 0;
    e->limits.sleep_slice_ms = ENGINE_DEFAULT_SLEEP_SLICE_MS;
    e->limits.source_bytes = 0;
    engine_refresh_limits(e);

    /* Every engine parses lang.aus for itself. That is what makes two
     * engines in one process independent: no class definition object is
     * ever shared, so nothing one engine does to a definition can be
     * seen by another. Ports Engine.parseLangSource. */
    parse_lang_source(e);
    e->init_complete = true;
    return e;
}

/* --- control: cancel, pause, resume ------------------------------ */

void engine_cancel(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    e->control = CONTROL_CANCELLED;
    /* Cancel outranks pause, so anything parked in a pause has to be
     * woken to find out it is being cancelled rather than resumed. */
    pthread_cond_broadcast(&e->control_cond);
    pthread_mutex_unlock(&e->control_lock);
}

bool engine_is_cancelled(const engine *e)
{
    return e->control == CONTROL_CANCELLED;
}

bool engine_clear_cancel(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    bool prev = (e->control == CONTROL_CANCELLED);
    if (prev) e->control = CONTROL_RUNNING;
    pthread_mutex_unlock(&e->control_lock);
    return prev;
}

void engine_pause(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    /* A cancelled engine stays cancelled: pausing a program that has
     * been told to stop would leave it parked forever. */
    if (e->control != CONTROL_CANCELLED) e->control = CONTROL_PAUSED;
    pthread_mutex_unlock(&e->control_lock);
}

void engine_resume(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    if (e->control == CONTROL_PAUSED) e->control = CONTROL_RUNNING;
    pthread_cond_broadcast(&e->control_cond);
    pthread_mutex_unlock(&e->control_lock);
}

control_state engine_get_control_state(const engine *e)
{
    return e->control;
}

/* Whether every registered thread has parked. Caller holds the lock. */
static bool all_parked_locked(const engine *e)
{
    /* No thread is running the engine, so nothing has stopped. Saying
     * "fully paused" here would tell a host its program had reached a
     * quiescent point when in fact it never started. */
    if (e->threads_count == 0) return false;
    for (size_t i = 0; i < e->threads_count; i++) {
        if (!e->threads[i]->parked) return false;
    }
    return true;
}

bool engine_is_fully_paused(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    bool ret = (e->control == CONTROL_PAUSED) && all_parked_locked(e);
    pthread_mutex_unlock(&e->control_lock);
    return ret;
}

bool engine_await_paused(engine *e, int64_t timeout_ms)
{
    /* A deadline past what time_t holds (a 32-bit time_t on armv7 runs
     * out in 2038) becomes the latest time_t instead of wrapping into
     * the past, as deadline_in in concurrent_methods.c does. */
    const time_t tmax = (time_t)((sizeof(time_t) == 8) ? INT64_MAX : INT32_MAX);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    int64_t sec = timeout_ms / 1000;
    long nsec = deadline.tv_nsec + (long)((timeout_ms % 1000) * 1000000L);
    if (nsec >= 1000000000L) { sec++; nsec -= 1000000000L; }
    if (sec > (int64_t)(tmax - deadline.tv_sec)) {
        deadline.tv_sec = tmax;
        deadline.tv_nsec = 999999999L;
    } else {
        deadline.tv_sec += (time_t)sec;
        deadline.tv_nsec = nsec;
    }

    pthread_mutex_lock(&e->control_lock);
    bool ret = (e->control == CONTROL_PAUSED) && all_parked_locked(e);
    while (!ret) {
        if (timeout_ms <= 0) break;
        /* Woken by a thread parking or leaving, which is exactly when
         * the answer can change. */
        int rc = pthread_cond_timedwait(&e->control_cond, &e->control_lock,
                                        &deadline);
        ret = (e->control == CONTROL_PAUSED) && all_parked_locked(e);
        if (rc == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&e->control_lock);
    return ret;
}

bool engine_await_resume_or_cancel(engine *e)
{
    /* Fast path: the state a running program is in. One read and a
     * compare, no lock. */
    if (e->control == CONTROL_RUNNING) return false;

    pthread_mutex_lock(&e->control_lock);

    /* Find this thread's registration so a waiter can be reported as
     * parked. A thread running the engine without registering (a
     * nested or synthetic evaluation) still waits correctly; it just
     * does not count toward engine_is_fully_paused. */
    thread_scope *self = NULL;
    for (size_t i = 0; i < e->threads_count; i++) {
        if (pthread_equal(e->threads[i]->thread, pthread_self())) {
            self = e->threads[i];
            break;
        }
    }

    if (self != NULL && e->control == CONTROL_PAUSED) {
        self->parked = true;
        /* Wake engine_await_paused: this thread parking may have been
         * the last one it was waiting for. */
        pthread_cond_broadcast(&e->control_cond);
    }
    while (e->control == CONTROL_PAUSED) {
        pthread_cond_wait(&e->control_cond, &e->control_lock);
    }
    if (self != NULL) self->parked = false;

    bool cancelled = (e->control == CONTROL_CANCELLED);
    pthread_mutex_unlock(&e->control_lock);
    return cancelled;
}

/* --- interpreter thread registration and accounting -------------- */

/* This thread's CPU time in nanoseconds, or 0 when the platform will
 * not report it. CLOCK_THREAD_CPUTIME_ID is the direct analogue of the
 * JVM's per-thread CPU counter. */
static uint64_t thread_cpu_nanos(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Another thread's CPU time in nanoseconds, or 0 when it cannot be
 * read. This is what lets a host watch a program it is not running:
 * asking "how much CPU has this engine used" is a question the host
 * thread asks about the interpreter thread, so reading only the
 * caller's own clock would always answer 0 while a program runs.
 *
 * Safe only while the caller holds control_lock. A thread is in the
 * registry exactly while it is inside its program body, and
 * engine_exit_interpreter_thread takes the same lock before removing
 * itself, so a registered thread cannot exit underneath this and
 * invalidate the clock id. */
#if defined(__APPLE__)
/* Apple platforms do not provide pthread_getcpuclockid at all, so
 * another thread's CPU time cannot be read this way. The contract above
 * already allows 0 for "cannot be read", so this returns 0 rather than
 * growing a Mach thread_info implementation. That is a deliberate
 * decision, not an oversight.
 * A host reading CPU usage on macOS or iOS therefore sees 0 while a
 * program runs on another thread; the calling thread's own clock, read
 * by thread_cpu_nanos above, still works. */
static uint64_t other_thread_cpu_nanos(pthread_t th)
{
    (void)th;
    return 0;
}
#else
static uint64_t other_thread_cpu_nanos(pthread_t th)
{
    clockid_t cid;
    if (pthread_getcpuclockid(th, &cid) != 0) return 0;
    struct timespec ts;
    if (clock_gettime(cid, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

thread_scope *engine_enter_interpreter_thread(engine *e)
{
    thread_scope *sc = AUSSOM_ALLOC(sizeof(*sc));
    sc->eng = e;
    sc->thread = pthread_self();
    sc->cpu_baseline = thread_cpu_nanos();
    sc->alloc_baseline = aussom_thread_alloc_bytes;
    /* Taken here, on the registering thread, so it names this thread's
     * counter rather than whoever later reads it. */
    sc->alloc_counter = &aussom_thread_alloc_bytes;
    sc->closed = false;
    sc->parked = false;

    pthread_mutex_lock(&e->control_lock);
    if (e->threads_count == e->threads_capacity) {
        size_t nc = (e->threads_capacity == 0) ? 4 : e->threads_capacity * 2;
        e->threads = AUSSOM_REALLOC(e->threads, nc * sizeof(*e->threads));
        e->threads_capacity = nc;
    }
    e->threads[e->threads_count++] = sc;
    pthread_mutex_unlock(&e->control_lock);
    return sc;
}

void engine_exit_interpreter_thread(thread_scope *sc)
{
    if (sc == NULL || sc->closed) return;
    engine *e = sc->eng;

    /* Read the counters before deregistering. This is the last chance
     * to attribute the work: a thread's CPU clock is gone once the
     * thread exits, and its allocation counter goes with it. */
    uint64_t cpu = thread_cpu_nanos();
    uint64_t alloc = aussom_thread_alloc_bytes;

    pthread_mutex_lock(&e->control_lock);
    sc->closed = true;
    if (cpu > sc->cpu_baseline) e->banked_cpu_nanos += cpu - sc->cpu_baseline;
    if (alloc > sc->alloc_baseline) {
        e->banked_alloc_bytes += alloc - sc->alloc_baseline;
    }
    /* The counter belongs to this thread, which is about to leave. */
    sc->alloc_counter = NULL;
    for (size_t i = 0; i < e->threads_count; i++) {
        if (e->threads[i] == sc) {
            e->threads[i] = e->threads[e->threads_count - 1];
            e->threads_count--;
            break;
        }
    }
    /* A thread leaving changes the answer engine_await_paused is
     * waiting on, the same way a thread parking does. */
    pthread_cond_broadcast(&e->control_cond);
    pthread_mutex_unlock(&e->control_lock);
}

size_t engine_interpreter_thread_count(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    size_t n = e->threads_count;
    pthread_mutex_unlock(&e->control_lock);
    return n;
}

/* This scope's CPU usage so far. Caller holds control_lock. */
static uint64_t scope_cpu(thread_scope *sc)
{
    uint64_t now;
    if (pthread_equal(sc->thread, pthread_self())) now = thread_cpu_nanos();
    else now = other_thread_cpu_nanos(sc->thread);
    if (now > sc->cpu_baseline) return now - sc->cpu_baseline;
    return 0;
}

/* This scope's allocation so far. Caller holds control_lock.
 *
 * Reading another thread's counter is a plain load of a 64-bit word
 * that thread is incrementing. On any platform with aligned 64-bit
 * loads that yields some value the counter genuinely held, which is
 * all an accounting figure needs; making it atomic would put a
 * synchronized write on every allocation to buy nothing. */
static uint64_t scope_alloc(thread_scope *sc)
{
    if (sc->alloc_counter == NULL) return 0;
    uint64_t now = *sc->alloc_counter;
    if (now > sc->alloc_baseline) return now - sc->alloc_baseline;
    return 0;
}

uint64_t engine_get_cpu_nanos(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    uint64_t total = e->banked_cpu_nanos;
    for (size_t i = 0; i < e->threads_count; i++) {
        total += scope_cpu(e->threads[i]);
    }
    pthread_mutex_unlock(&e->control_lock);
    return total;
}

uint64_t engine_get_allocated_bytes(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    uint64_t total = e->banked_alloc_bytes;
    for (size_t i = 0; i < e->threads_count; i++) {
        total += scope_alloc(e->threads[i]);
    }
    pthread_mutex_unlock(&e->control_lock);
    return total;
}

void engine_reset_accounting(engine *e)
{
    pthread_mutex_lock(&e->control_lock);
    e->banked_cpu_nanos = 0;
    e->banked_alloc_bytes = 0;
    for (size_t i = 0; i < e->threads_count; i++) {
        thread_scope *sc = e->threads[i];
        if (pthread_equal(sc->thread, pthread_self())) {
            sc->cpu_baseline = thread_cpu_nanos();
        } else {
            sc->cpu_baseline = other_thread_cpu_nanos(sc->thread);
        }
        if (sc->alloc_counter != NULL) sc->alloc_baseline = *sc->alloc_counter;
    }
    pthread_mutex_unlock(&e->control_lock);
}

/* See the header: deliberately not implemented until the project's own
 * allocator replaces Boehm and can answer this directly. */
int64_t engine_measure_retained_footprint(engine *e)
{
    (void)e;
    return ENGINE_FOOTPRINT_UNAVAILABLE;
}

int64_t engine_get_class_definition_bytes(engine *e)
{
    (void)e;
    return ENGINE_FOOTPRINT_UNAVAILABLE;
}

/* --- limits ------------------------------------------------------ */

/* One limit from policy. A negative value has no meaning as a limit,
 * so it falls back to the default rather than being honored. */
static int64_t read_limit(engine *e, const char *name, int64_t dflt)
{
    int64_t v = security_manager_get_property_int(e->secman, name, (int)dflt);
    if (v < 0) return dflt;
    return v;
}

void engine_refresh_limits(engine *e)
{
    e->limits.call_depth =
        read_limit(e, ENGINE_LIMIT_CALL_DEPTH_PROP, ENGINE_DEFAULT_CALL_DEPTH);
    e->limits.regex_steps = read_limit(e, ENGINE_LIMIT_REGEX_STEPS_PROP, 0);
    e->limits.sleep_slice_ms =
        read_limit(e, ENGINE_LIMIT_SLEEP_SLICE_PROP, ENGINE_DEFAULT_SLEEP_SLICE_MS);
    e->limits.source_bytes = read_limit(e, ENGINE_LIMIT_SOURCE_BYTES_PROP, 0);
}

const engine_limits *engine_get_limits(const engine *e)
{
    return &e->limits;
}

int64_t engine_get_max_call_depth(const engine *e)
{
    return e->limits.call_depth;
}

struct environment *engine_get_script_environment(const engine *e)
{
    return e->script_env;
}

struct security_manager *engine_get_security_manager(const engine *e)
{
    return e->secman;
}

/* --- extern class allowlist -------------------------------------- */

bool engine_is_extern_class_allowed(engine *e, const char *class_name)
{
    if (!security_manager_get_property_boolean(e->secman,
            "aussom.extern.allowlist.enforce", false)) {
        return true;
    }
    if (class_name == NULL) return false;

    /* Enforcement on with no usable list denies everything. Switching
     * the control on must never permit more than intended. */
    av_list *allowed = security_manager_get_property_list(e->secman,
                                                          "aussom.extern.allowed");
    if (allowed == NULL) return false;

    size_t n = av_list_size(allowed);
    for (size_t i = 0; i < n; i++) {
        av_type *entry = av_list_get(allowed, i);
        if (entry == NULL || entry->type == C_TYPE_NULL) continue;
        /* Java's entry.toString().trim(): trims characters up to and
         * including the space from both ends. */
        const char *ent = av_str(entry);
        size_t len = strlen(ent);
        while (len > 0 && (unsigned char)ent[0] <= ' ') { ent++; len--; }
        while (len > 0 && (unsigned char)ent[len - 1] <= ' ') len--;
        if (len == 0) continue;
        if (len >= 2 && ent[len - 2] == '.' && ent[len - 1] == '*') {
            /* Drop only the star and keep the trailing dot, so an entry
             * of 'com.a.std.*' does not admit 'com.a.stdlib.Foo'. */
            if (strncmp(class_name, ent, len - 1) == 0) return true;
        } else if (strlen(class_name) == len && strncmp(ent, class_name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* Builds and registers the singleton for static class `cls`. Ports
 * Engine.instantiateStaticClass, including its synthetic frame. Java
 * throws aussomException(class, text, stack trace) when the
 * instantiation fails, which stops the parse that added the class; here
 * the failure is left as the engine's parse exception, and the grammar
 * action that added the class stops the parse. */
static void instantiate_static_class(engine *e, ast_class *cls)
{
    environment *env = environment_new(e);
    callstack *frame = callstack_new_frame(cls->base.file_name,
                                           cls->base.line_num, cls->base.name,
                                           "<static-init>",
                                           "Static class initializer.");
    environment_set(env, NULL, av_strmap_new_unlocked(), frame);
    av_type *inst = class_instantiate_static_singleton(env, cls, false,
                                                       av_list_new());
    if (av_is_ex(inst)) {
        const char *text = av_exception_get_text((av_exception *)inst);
        const char *trace = av_exception_get_stack_trace((av_exception *)inst);
        const char *file = cls->base.file_name != NULL ? cls->base.file_name : "";
        if (text == NULL) text = "";
        if (trace == NULL) trace = "";
        size_t n = strlen(file) + strlen(text) + strlen(trace) + 32;
        char *full = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(full, n, "%s [%d]: %s\n%s", file, cls->base.line_num, text, trace);
        /* getAussomStackTrace(): the text, then the trace. */
        char *st = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(st, n, "%s\n%s", text, trace);
        size_t sl = strlen(st);
        while (sl > 0 && st[sl - 1] == '\n') st[--sl] = '\0';
        engine_set_parse_exception(e, full, st);
        return;
    }
    engine_register_static(e, cls->base.name, inst);
}

void engine_add_class(engine *e, ast_class *cls)
{
    ast_node_array_add(&e->classes, &cls->base);
    if (e->init_complete && ast_class_get_static(cls)) {
        instantiate_static_class(e, cls);
    }
}

/* Appends `path` to a growable string array, adding a trailing '/'
 * when absent. Ports the shared body of Engine.addIncludePath and
 * Engine.addExcludePath. */
static void add_search_path(char ***arr, size_t *count, size_t *capacity,
                            const char *path)
{
    if (*count == *capacity) {
        size_t new_cap = (*capacity == 0) ? 4 : *capacity * 2;
        *arr = AUSSOM_REALLOC(*arr, new_cap * sizeof(**arr));
        *capacity = new_cap;
    }

    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/') {
        (*arr)[(*count)++] = AUSSOM_STRDUP(path);
    } else {
        char *with_sep = AUSSOM_ALLOC_ATOMIC(len + 2);
        memcpy(with_sep, path, len);
        with_sep[len] = '/';
        with_sep[len + 1] = '\0';
        (*arr)[(*count)++] = with_sep;
    }
}

void engine_add_include_path(engine *e, const char *path)
{
    add_search_path(&e->include_paths, &e->include_paths_count,
                    &e->include_paths_capacity, path);
}

size_t engine_include_path_count(const engine *e)
{
    return e->include_paths_count;
}

const char *engine_get_include_path(const engine *e, size_t i)
{
    return e->include_paths[i];
}

void engine_add_exclude_path(engine *e, const char *path)
{
    add_search_path(&e->exclude_paths, &e->exclude_paths_count,
                    &e->exclude_paths_capacity, path);
}

size_t engine_exclude_path_count(const engine *e)
{
    return e->exclude_paths_count;
}

const char *engine_get_exclude_path(const engine *e, size_t i)
{
    return e->exclude_paths[i];
}

bool engine_is_path_exclude_path(const engine *e, const char *test_path)
{
    for (size_t i = 0; i < e->exclude_paths_count; i++) {
        if (strncmp(test_path, e->exclude_paths[i],
                    strlen(e->exclude_paths[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* --- base language bootstrap -------------------------------------- */

/* Instantiates this engine's static classes. Two passes, because the
 * second needs the first: a bare dispatch anchor for every static class
 * first, then a full instantiation for the ones that declare members
 * (the `dateunit` enum, and any other constant-bearing static class),
 * whose initializers can then reach the other statics and call extern
 * methods. Method-only statics (c, lang, json, ...) keep their anchor. */
static void instantiate_static_classes(engine *e)
{
    size_t n = engine_class_count(e);
    for (size_t i = 0; i < n; i++) {
        ast_class *cls = engine_get_class(e, i);
        if (!ast_class_get_static(cls)) continue;
        av_object *inst = av_object_new();
        av_object_set_class_def(inst, (struct ast_class *)cls);
        av_object_set_extern_object(inst, inst);
        engine_register_static(e, cls->base.name, &inst->base);
    }

    for (size_t i = 0; i < n; i++) {
        ast_class *cls = engine_get_class(e, i);
        if (!ast_class_get_static(cls) || cls->members.count == 0) continue;
        environment *env = environment_new(e);
        /* Engine.instantiateStaticClass's frame, as for any static. */
        callstack *root = callstack_new_frame(cls->base.file_name,
                                              cls->base.line_num, cls->base.name,
                                              "<static-init>",
                                              "Static class initializer.");
        environment_set(env, NULL, av_strmap_new_unlocked(), root);
        av_type *inst = class_instantiate_static_singleton(env, cls, false,
                                                           av_list_new());
        if (av_is_ex(inst)) continue;
        engine_register_static(e, cls->base.name, inst);
    }
}

/* Parses lang.aus into this engine's class table, caches the primitive
 * definitions for the dispatch path, and instantiates the statics.
 *
 * A failure here means the embedded standard library did not parse,
 * which is a broken build rather than a runtime condition: the same
 * bytes ship in every copy of the binary. It is still reported the way
 * every other parse failure is -- the sticky parse-error flag plus a
 * diagnostic -- so a host reads it back through the channel it already
 * uses, and engine_run refuses to run the engine either way. */
static void parse_lang_source(engine *e)
{
    const char *src = engine_module_source(e, "lang.aus");
    if (src == NULL || !aussom_parse_string(e, src, "lang.aus")) {
        engine_add_parse_diagnostic(e, "lang.aus", PARSE_DIAG_NO_POSITION,
            PARSE_DIAG_NO_POSITION,
            "Engine: the embedded standard library did not parse. This "
            "engine has no base language classes.");
        engine_set_parse_error(e);
        engine_err(e, "Engine: the embedded standard library ('lang.aus') "
                      "did not parse. This build is broken.");
        return;
    }

    static const struct { c_type t; const char *name; } prim[] = {
        { C_TYPE_NULL,      "cnull" },
        { C_TYPE_BOOL,      "bool" },
        { C_TYPE_INT,       "int" },
        { C_TYPE_DOUBLE,    "double" },
        { C_TYPE_STRING,    "string" },
        { C_TYPE_LIST,      "list" },
        { C_TYPE_MAP,       "map" },
        { C_TYPE_OBJECT,    "object" },
        { C_TYPE_CALLBACK,  "callback" },
        { C_TYPE_EXCEPTION, "exception" },
    };
    for (size_t i = 0; i < sizeof(prim) / sizeof(prim[0]); i++) {
        e->type_defs[prim[i].t] = engine_get_class_by_name(e, prim[i].name);
    }

    instantiate_static_classes(e);
}

struct ast_class *engine_primitive_class_def(const engine *e, c_type t)
{
    if ((int)t < 0 || (size_t)t >= sizeof(e->type_defs) / sizeof(e->type_defs[0])) {
        return NULL;
    }
    return e->type_defs[t];
}

/* --- standard library modules ------------------------------------ */

/* Finds `name` in the registry, or -1. Linear over a handful of
 * entries, and only ever reached during include resolution rather than
 * on a dispatch path, so a hash table would buy nothing. */
static long module_index(const engine *e, const char *name)
{
    for (size_t i = 0; i < e->modules_count; i++) {
        if (strcmp(e->modules[i].name, name) == 0) return (long)i;
    }
    return -1;
}

static void module_put(engine *e, const char *name, const char *src)
{
    long at = module_index(e, name);
    if (at >= 0) {                      /* replace, matching Map.put */
        e->modules[at].src = src;
        return;
    }
    if (e->modules_count == e->modules_capacity) {
        size_t nc = (e->modules_capacity == 0) ? 16 : e->modules_capacity * 2;
        e->modules = AUSSOM_REALLOC(e->modules, nc * sizeof(*e->modules));
        e->modules_capacity = nc;
    }
    e->modules[e->modules_count].name = name;
    e->modules[e->modules_count].src = src;
    e->modules_count++;
}

/* Gives a new engine the base standard library. The name and source
 * pointers are stored as-is: they address read-only program memory that
 * outlives every engine, so there is nothing to copy and nothing to
 * own. */
static void seed_base_modules(engine *e)
{
    size_t n = stdlib_source_count();
    for (size_t i = 0; i < n; i++) {
        module_put(e, stdlib_source_name(i), stdlib_source_at(i));
    }
}

void engine_add_module(engine *e, const char *name, const char *source)
{
    if (name == NULL || source == NULL) return;
    module_put(e, AUSSOM_STRDUP(name), AUSSOM_STRDUP(source));
}

const char *engine_module_source(const engine *e, const char *name)
{
    long at = module_index(e, name);
    return (at >= 0) ? e->modules[at].src : NULL;
}

bool engine_has_module(const engine *e, const char *name)
{
    return module_index(e, name) >= 0;
}

size_t engine_module_count(const engine *e)
{
    return e->modules_count;
}

const char *engine_module_name(const engine *e, size_t i)
{
    return (i < e->modules_count) ? e->modules[i].name : NULL;
}

/* --- extern packages ---------------------------------------------- */

/* The package named `name`, or NULL. Caller holds extern_packages_lock. */
static extern_package *find_package_locked(engine *e, const char *name,
                                           size_t name_len)
{
    for (size_t i = 0; i < e->extern_packages_count; i++) {
        const char *pn = extern_package_name(e->extern_packages[i]);
        if (strlen(pn) == name_len && strncmp(pn, name, name_len) == 0) {
            return e->extern_packages[i];
        }
    }
    return NULL;
}

bool engine_add_extern_package(engine *e, const aussom_extern_package *pkg)
{
    /* Copied outside the lock: the copy touches nothing shared. */
    extern_package *copy = extern_package_copy(pkg);
    if (copy == NULL) return false;
    const char *name = extern_package_name(copy);

    pthread_mutex_lock(&e->extern_packages_lock);
    if (find_package_locked(e, name, strlen(name)) != NULL) {
        pthread_mutex_unlock(&e->extern_packages_lock);
        return false;
    }
    if (e->extern_packages_count == e->extern_packages_capacity) {
        size_t nc = (e->extern_packages_capacity == 0) ? 4 : e->extern_packages_capacity * 2;
        e->extern_packages = AUSSOM_REALLOC(e->extern_packages,
                                            nc * sizeof(*e->extern_packages));
        e->extern_packages_capacity = nc;
    }
    e->extern_packages[e->extern_packages_count++] = copy;
    pthread_mutex_unlock(&e->extern_packages_lock);
    return true;
}

const extern_class *engine_find_extern_class(engine *e, const char *ref)
{
    const char *dot = strrchr(ref, '.');
    if (dot == NULL) return NULL;

    pthread_mutex_lock(&e->extern_packages_lock);
    extern_package *pkg = find_package_locked(e, ref, (size_t)(dot - ref));
    pthread_mutex_unlock(&e->extern_packages_lock);
    if (pkg == NULL) return NULL;
    return extern_package_find_class(pkg, dot + 1);
}

void engine_set_logger(engine *e, const console_logger *logger)
{
    e->logger = logger;
}

const console_logger *engine_get_logger(const engine *e)
{
    return e->logger;
}

void engine_set_log_level(engine *e, console_level level)
{
    e->log_level = level;
}

console_level engine_get_log_level(const engine *e)
{
    return e->log_level;
}

void engine_set_parse_error(engine *e)
{
    e->parse_error = true;
}

void engine_clear_parse_error(engine *e)
{
    e->parse_error = false;
}

bool engine_has_parse_error(const engine *e)
{
    return e->parse_error;
}

const char *engine_parse_exception(const engine *e)
{
    return e->parse_exception;
}

const char *engine_parse_exception_stack_trace(const engine *e)
{
    return e->parse_exception_stack_trace;
}

void engine_set_parse_exception(engine *e, const char *message,
                                const char *aussom_stack_trace)
{
    e->parse_exception = AUSSOM_STRDUP(message != NULL ? message : "");
    e->parse_exception_stack_trace =
        AUSSOM_STRDUP(aussom_stack_trace != NULL ? aussom_stack_trace : "");
}

void engine_clear_parse_exception(engine *e)
{
    e->parse_exception = NULL;
    e->parse_exception_stack_trace = NULL;
}

void engine_add_parse_diagnostic(engine *e, const char *file_name, int line,
                                 int col, const char *message)
{
    if (e->diags_count == e->diags_capacity) {
        size_t nc = (e->diags_capacity == 0) ? 8 : e->diags_capacity * 2;
        e->diags = AUSSOM_REALLOC(e->diags, nc * sizeof(*e->diags));
        e->diags_capacity = nc;
    }
    parse_diagnostic *d = &e->diags[e->diags_count++];
    d->file_name = AUSSOM_STRDUP(file_name != NULL ? file_name : "");
    d->line = line;
    d->col = col;
    d->severity = PARSE_DIAG_SEVERITY_ERROR;
    d->message = AUSSOM_STRDUP(message != NULL ? message : "");
}

size_t engine_parse_diagnostic_count(const engine *e)
{
    return e->diags_count;
}

const parse_diagnostic *engine_get_parse_diagnostic(const engine *e, size_t i)
{
    if (i >= e->diags_count) return NULL;
    return &e->diags[i];
}

void engine_clear_parse_diagnostics(engine *e)
{
    e->diags_count = 0;
}

size_t engine_class_count(const engine *e)
{
    return e->classes.count;
}

ast_class *engine_get_class(const engine *e, size_t i)
{
    return (ast_class *)e->classes.items[i];
}

ast_class *engine_get_class_by_name(const engine *e, const char *name)
{
    for (size_t i = 0; i < e->classes.count; i++) {
        ast_class *c = (ast_class *)e->classes.items[i];
        if (strcmp(c->base.name, name) == 0) return c;
    }
    return NULL;
}

bool engine_contains_class(const engine *e, const char *name)
{
    return engine_get_class_by_name(e, name) != NULL;
}

/* This engine's classes only. There is no process-wide fallback: every
 * engine parses lang.aus for itself, so the base language classes are
 * already in the table above, and they are THIS engine's copies. That
 * is the whole point of the isolation -- a definition one engine can
 * reach is never one another engine could have altered. */
ast_class *engine_resolve_class(const engine *e, const char *name)
{
    return engine_get_class_by_name(e, name);
}

void engine_register_static(engine *e, const char *name, struct av_type *inst)
{
    if (e->static_classes == NULL) e->static_classes = av_strmap_new();
    av_strmap_put(e->static_classes, name, inst);
}

/* This engine's static instances only, for the same reason as
 * engine_resolve_class: the stdlib statics (`c`, `math`, `sys`, ...)
 * were instantiated into this engine by engine_new. */
struct av_type *engine_get_static_class(const engine *e, const char *name)
{
    if (e->static_classes == NULL) return NULL;
    return av_strmap_get(e->static_classes, name);
}

bool engine_contains_static_class(const engine *e, const char *name)
{
    return engine_get_static_class(e, name) != NULL;
}

size_t engine_include_count(const engine *e)
{
    return e->includes_count;
}

const char *engine_get_include(const engine *e, size_t i)
{
    return e->includes[i];
}

/* --- script entry ------------------------------------------------ */

av_type *eval_run_statements(engine *eng, struct ast_statement_list *script,
                             struct av_strmap *locals)
{
    /* Engine.evalParsedScript's gate, checked on every entry. */
    if (!security_manager_get_property_boolean(eng->secman, "aussom.script.mode.enable", false)) {
        return &av_exception_new("eval_run_statements: Security exception, action "
                                 "'aussom.script.mode.enable' not permitted.")->base.base;
    }
    /* Policy may have changed since the engine was built. */
    engine_refresh_limits(eng);
    environment *env = environment_new(eng);
    /* Java's script environment starts from an empty call stack, which
     * adds nothing to a trace. */
    callstack *root = callstack_new();
    environment_set(env, NULL, (av_strmap *)locals, root);
    /* Retained so an embedder can read session scope back out; see
     * engine_get_script_environment. */
    eng->script_env = env;
    return ast_node_eval(&((ast_statement_list *)script)->base, env, false);
}

/* --- program entry ----------------------------------------------- */

/* True when `cls` declares a function named `main`. Mirrors
 * astClass.hasAnyFunctionByName("main") as used by
 * Engine.setMainClassAndFunct. */
static bool class_declares_main(const ast_class *cls)
{
    return ast_class_has_any_function_by_name(cls, "main");
}

/* True when `cls` has any single-argument `main` overload. This covers
 * main(list args), untyped main(args), variadic main(...) and optional
 * main(args = null), matching the getFunctionsByName("main") and
 * getMinArity/getMaxArity test in Engine.callMain. */
static bool class_declares_one_arg_main(const ast_class *cls)
{
    size_t n;
    ast_funct_def **fl = ast_class_get_functions_by_name(cls, "main", &n);
    for (size_t i = 0; i < n; i++) {
        if (ast_funct_def_min_arity(fl[i]) <= 1 &&
            ast_funct_def_max_arity(fl[i]) >= 1) {
            return true;
        }
    }
    return false;
}

/* The first class in the engine declaring a `main` function, or NULL.
 * Mirrors Engine.setMainClassAndFunct. */
static ast_class *find_main_class(const engine *e)
{
    size_t n = engine_class_count(e);
    for (size_t i = 0; i < n; i++) {
        ast_class *c = engine_get_class(e, i);
        if (class_declares_main(c)) return c;
    }
    return NULL;
}

int engine_run(engine *e, const char *program_name)
{
    /* Java throws here; C has no exception to propagate to a host, so
     * the failure is logged and reported through the return value. The
     * text ends in a newline because Java's Main prints the throw as
     * getAussomStackTrace(): the message, a newline, and an empty
     * trace. */
    if (engine_has_parse_error(e)) {
        engine_err(e, "Engine.run(): Parse errors were encountered. Not running.\n");
        return 1;
    }

    /* Policy may have been rewritten between construction and the
     * program starting; this is the "start of each program" refresh. */
    engine_refresh_limits(e);

    ast_class *main_cls = find_main_class(e);
    if (main_cls == NULL) {
        engine_err(e, "Engine.run(): Failed to find main class.\n");
        return 1;
    }

    /* Register this thread as the one running the engine's code, for
     * the length of the program body. That is what lets a host see the
     * program's CPU and allocation usage, and what makes a pause
     * report as fully paused once the thread parks. */
    thread_scope *scope = engine_enter_interpreter_thread(e);

    environment *env = environment_new(e);
    /* Engine.callMain runs under the engine's main call stack, an empty
     * root that adds nothing to a trace; main's own "Defined." frame is
     * the first one shown. */
    (void)program_name;
    callstack *root = callstack_new();
    environment_set(env, NULL, av_strmap_new_unlocked(), root);

    /* A static main class was already built once during the static
     * startup pass, so reuse that singleton rather than instantiating a
     * second, detached copy. */
    av_type *tci;
    if (ast_class_get_static(main_cls)
        && engine_get_static_class(e, main_cls->base.name) != NULL) {
        tci = engine_get_static_class(e, main_cls->base.name);
    } else {
        tci = class_instantiate(env, main_cls, false, av_list_new());
    }
    if (av_is_ex(tci)) {
        engine_err(e, av_str(tci));
        engine_exit_interpreter_thread(scope);
        return 1;
    }
    av_object *inst = (av_object *)tci;

    /* Pass the args list only when the program declared a 1-arg main,
     * so a script defining only main() does not trip FUNCT_NOT_FOUND. */
    av_list *margs = av_list_new();
    if (class_declares_one_arg_main(main_cls)) {
        av_list_add(margs, &av_list_new()->base.base);
    }

    environment *tenv = environment_clone(env, &inst->base);
    av_type *ret = class_call(tenv, false, main_cls, "main", margs);

    /* Banks this thread's CPU and allocation usage. Done before the
     * returns below because it is the last chance to attribute it. */
    engine_exit_interpreter_thread(scope);

    if (av_is_ex(ret)) {
        engine_err(e, av_str(ret));
        return 1;
    }
    if (ret->type == C_TYPE_INT) {
        return (int)av_int_get((av_int *)ret);
    }
    return 0;
}
