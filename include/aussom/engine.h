/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_ENGINE_H
#define AUSSOM_ENGINE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aussom/ast_class.h"
#include "aussom/ast_node.h"
#include "aussom/console.h"
#include "aussom/debugger.h"
#include "aussom/extern_registry.h"

struct av_type;     /* for static-class instance lookups */
struct av_strmap;   /* per-engine static-class instance registry */
struct av_list;     /* main's argument list */
struct security_manager; /* the engine's policy, security_manager.h */

/* What a running Aussom program should do when it reaches its next
 * checkpoint. The engine holds one of these in a volatile field, so
 * the common case costs a single read and a compare.
 *
 * Checkpoints are the loop back edges, every Aussom function call,
 * every batch of regex subject reads, and every sleep slice. Mirrors
 * com.aussom.ControlState. */
typedef enum control_state {
    /* Keep going. The state a program runs in. */
    CONTROL_RUNNING = 0,

    /* Stop at the next checkpoint and block there until the state
     * becomes RUNNING or CANCELLED. The program keeps its stack, its
     * locals and its data, so resuming continues where it stopped. */
    CONTROL_PAUSED,

    /* Stop at the next checkpoint and unwind with the cancellation
     * exception. Outranks PAUSED: a paused program that is cancelled
     * ends without having to be resumed first. */
    CONTROL_CANCELLED
} control_state;

/* Numeric runtime settings for one engine, read from its security
 * properties. Mirrors com.aussom.Limits. A snapshot, rebuilt by
 * engine_refresh_limits, so the interpreter never pays for a map
 * lookup on the hot path.
 *
 * Each bounds something the C runtime will not bound for you:
 *
 *   - call_depth: how deep Aussom calls may nest. This matters more
 *     here than on the JVM. Java gets a StackOverflowError it can
 *     convert into an exception; C gets SIGSEGV and the process dies.
 *     Measured on this interpreter: about 1.35 KB of native stack per
 *     Aussom call, so an 8 MB main thread runs out at roughly 6200
 *     nested calls and a 1 MB embedder thread at roughly 760. Running
 *     out no longer crashes: the native stack guard (stack_guard.h)
 *     raises STACK_OVERFLOW first. The limit keeps Java's behavior and
 *     error on stacks large enough to reach it.
 *
 *   - regex_steps: how much work one regular expression may do,
 *     applied as the PCRE2 match limit. 0 for no budget. Without it a
 *     short pattern can burn a core for a minute on a short subject.
 *
 *   - sleep_slice_ms: how long a sleeping program may run before the
 *     interpreter reads the control state again, which is the ceiling
 *     on how long a pause or a cancel waits to take effect. 0 turns
 *     slicing off and restores one uninterruptible wait.
 *
 *   - source_bytes: the largest source file the engine will parse,
 *     checked against the file length before it is read. 0 for no
 *     limit.
 *
 * There are deliberately no per-value size caps (string length, list
 * elements, buffer bytes). A cap on one value bounds neither memory
 * nor anything else a host can reason about: element count is not
 * memory, and a script under any per-value cap can hold thousands of
 * values. Argument validation is a different matter and stayed --
 * buffer_methods refuses a negative or unaddressable size. */
typedef struct engine_limits {
    int64_t call_depth;
    int64_t regex_steps;
    int64_t sleep_slice_ms;
    int64_t source_bytes;
} engine_limits;

/* Security property names for the limits above. */
#define ENGINE_LIMIT_CALL_DEPTH_PROP   "aussom.limit.call.depth"
#define ENGINE_LIMIT_REGEX_STEPS_PROP  "aussom.limit.regex.steps"
#define ENGINE_LIMIT_SLEEP_SLICE_PROP  "aussom.limit.sleep.slice"
#define ENGINE_LIMIT_SOURCE_BYTES_PROP "aussom.limit.source.bytes"

/* Call depth used when policy says nothing. Kept at the Java value so
 * the two implementations agree. Note the measurement in the
 * engine_limits comment: on a thread stack too small for this many
 * calls, the native stack guard fires first and the program gets
 * STACK_OVERFLOW instead of CALL_DEPTH_EXCEEDED (see stack_guard.h). */
#define ENGINE_DEFAULT_CALL_DEPTH 1000

/* Milliseconds a sleeping program may run between control checks.
 * The one setting here where 0 means "no slicing" rather than "no
 * limit". */
#define ENGINE_DEFAULT_SLEEP_SLICE_MS 50

/* One thread's registration with an engine, for the length of one
 * program body. Mirrors com.aussom.ThreadScope.
 *
 * Two jobs. It tells the engine which threads are running its code,
 * which is what lets engine_is_fully_paused know when everything has
 * actually stopped. And it records the thread's CPU and allocation
 * counters on entry so the difference can be banked on the way out --
 * required rather than tidy, because a thread's counters are gone once
 * it exits. */
typedef struct thread_scope {
    struct engine *eng;
    pthread_t thread;
    uint64_t cpu_baseline;
    uint64_t alloc_baseline;

    /* Address of the registering thread's own allocation counter.
     * Taken in that thread, where the thread-local resolves to its own
     * slot, so another thread can read a running program's allocation
     * total. Valid for as long as the thread is registered, which is
     * strictly shorter than the thread's life. */
    uint64_t *alloc_counter;
    bool closed;
    /* True while this thread is blocked in a checkpoint waiting for a
     * resume. engine_is_fully_paused reports true when every open
     * scope has this set. */
    volatile bool parked;
} thread_scope;

/* One entry in an engine's module registry. Both pointers are borrowed
 * for base modules (read-only program memory) and GC-copied for ones a
 * host added, so neither is ever freed. */
typedef struct engine_module {
    const char *name;
    const char *src;
} engine_module;

/* The interpreter engine. Ports com.aussom.Engine: the parsed classes
 * and includes, the security manager, the logger, the standard library
 * modules and extern packages, the control state, the thread registry
 * and the debugger.
 *
 * Allocated via the project GC; do not free. */
typedef struct engine {
    /* Top-level classes parsed from input. Stored as ast_node *
     * holding ast_class * via the embedded base struct. */
    ast_node_array classes;

    /* The includes loaded so far. Ports Engine.includes. */
    char **includes;
    size_t includes_count;
    size_t includes_capacity;

    /* Filesystem search paths for includes not found in the bundled
     * standard library. Ports Engine.includePaths; each entry ends in
     * a '/' separator, applied by engine_add_include_path. */
    char **include_paths;
    size_t include_paths_count;
    size_t include_paths_capacity;

    /* Paths an include may not be loaded from. Ports
     * Engine.excludePaths; a candidate starting with any entry is
     * refused. */
    char **exclude_paths;
    size_t exclude_paths_count;
    size_t exclude_paths_capacity;

    /* True after any parser action calls engine_set_parse_error.
     * The parser entry points return false when this is set so
     * callers can avoid running broken AST. */
    bool parse_error;

    /* The error the last parse stopped with, where aussom-base throws
     * aussomException out of the parse. NULL when it did not stop that
     * way. `parse_exception` is the exception's getMessage() text and
     * `parse_exception_stack_trace` its getAussomStackTrace() text. See
     * engine_parse_exception. */
    char *parse_exception;
    char *parse_exception_stack_trace;

    /* Set once engine_new has parsed lang.aus and instantiated its
     * static classes. From then on engine_add_class instantiates a
     * static class as soon as it is added. Ports Engine.initComplete. */
    bool init_complete;

    /* Static-class instances declared in this engine (and its
     * includes), name -> instance. Populated by engine_new for lang.aus
     * and by engine_add_class after that. Lazily allocated. */
    struct av_strmap *static_classes;

    /* The security manager this engine was built with. Ports
     * Engine.secman: the engine holds the host's object, not a copy. */
    struct security_manager *secman;

    /* Where this engine writes its output. NULL means the built-in
     * default implementation, gated by log_level below. Ports the
     * per-Engine `LoggingInt logger` field: output belongs to the
     * engine that produced it, not to the thread that happened to run
     * it, so two engines in one process never cross-route and an engine
     * driven from a thread pool logs to one place from all of them.
     *
     * A host-installed logger owns its own gating, exactly as in Java
     * where setLevel lives on DefaultLoggingImpl rather than on
     * LoggingInt; log_level therefore applies only to the built-in
     * default. Two engines can share that default at different
     * verbosities because the gate is read from the engine, not from
     * the process-wide setting. */
    const console_logger *logger;
    console_level log_level;

    /* The standard library modules this engine can include, name ->
     * source. Ports LangRegistry: an instance, not a singleton, so two
     * engines can be given different standard libraries -- a sandboxed
     * engine a smaller set than a trusted one. A process-wide table
     * made that impossible and meant a late registration silently
     * changed what an already-running engine could include.
     *
     * Seeded from the embedded base library, whose entries point
     * straight at read-only program memory; only host-added entries are
     * copied. See stdlib_sources.h for why sharing the text is safe. */
    struct engine_module *modules;
    size_t modules_count;
    size_t modules_capacity;

    /* The extern packages this engine can bind classes to. Nothing here
     * is shared with another engine. A host may add a package at any
     * time, from any thread, so the list is guarded by
     * extern_packages_lock. A package is never changed or replaced once
     * added, so a class bound to one is read without the lock. */
    struct extern_package **extern_packages;
    size_t extern_packages_count;
    size_t extern_packages_capacity;
    pthread_mutex_t extern_packages_lock;

    /* This engine's own definitions for the primitive types, indexed by
     * c_type. Populated by engine_new once lang.aus has been parsed
     * into the table above. Ports the ten direct fields on the Java
     * Engine (STRING_CLASS_DEF and friends): a primitive carries no
     * definition, so dispatch resolves it here, and each engine answers
     * with its own -- which is what keeps two engines in one process
     * from sharing one. A field read, so the lookup stays as cheap as
     * the global one it replaced. */
    struct ast_class *type_defs[16];

    /* Structured parse diagnostics collected during parsing. These
     * carry the source position as integers so tooling (an LSP server,
     * a debug adapter, a notebook host) does not have to recover it
     * with regular expressions over the console output. Populated
     * alongside -- never instead of -- the existing messages. */
    struct parse_diagnostic *diags;
    size_t diags_count;
    size_t diags_capacity;

    /* Script-mode state. Ports the fields of the same names on the
     * Java Engine; see the Script mode section below. The script class
     * is deliberately not in the classes table, so the classic run
     * path never sees it. */
    bool script_mode;
    struct ast_class *script_class;
    /* The script class's main(args), held directly: its one untyped
     * argument files it among the wildcard overloads, where a lookup
     * by exact signature would not find it. */
    struct ast_funct_def *script_main_fn;
    struct av_object *script_instance;
    struct environment *script_env;
    /* The file name script-mode parses give their nodes; "<script>"
     * by default. */
    char *script_file_name;
    /* Index of the first statement in the script main's body not yet
     * evaluated. engine_eval_parsed_script advances it to the end of
     * the body, so a statement from an earlier call never runs again. */
    size_t script_cursor;

    /* What a running program should do at its next checkpoint. Written
     * by engine_cancel / engine_pause / engine_resume from any thread
     * and read at every checkpoint. Volatile because the thread that
     * writes it is never the interpreter thread. See the Control
     * section below. */
    volatile control_state control;

    /* Guards the pause handshake. A paused interpreter thread waits on
     * control_cond; engine_resume and engine_cancel signal it.
     * control_lock also guards the thread registry below. */
    pthread_mutex_t control_lock;
    pthread_cond_t control_cond;

    /* Threads currently running this engine's code, one entry per open
     * thread_scope. Used by engine_is_fully_paused and by the CPU and
     * allocation accounting. */
    thread_scope **threads;
    size_t threads_count;
    size_t threads_capacity;

    /* CPU nanoseconds and allocated bytes banked by threads that have
     * finished running this engine's code. A live thread's usage is
     * added to these on read; see engine_get_cpu_nanos. */
    uint64_t banked_cpu_nanos;
    uint64_t banked_alloc_bytes;

    /* Numeric limits snapshot, rebuilt by engine_refresh_limits. */
    engine_limits limits;

    /* The strings handed to main(args). Ports Engine.mainFunctArgs;
     * filled by engine_add_main_arg and engine_add_main_args. */
    struct av_list *main_funct_args;

    /* Debugging state. Ports Engine.debugMode and Engine.debugger; see
     * the Debugging section below.
     *
     * debug_mode is a plain bool because it is set before any
     * interpreter thread starts and is read before every node, so it
     * has to be cheap. debugger is volatile because a debug session may
     * swap or clear it while interpreter threads run. The pointer is
     * borrowed: the host keeps the table alive while it is installed. */
    bool debug_mode;
    const aussom_debugger *volatile debugger;
} engine;

/* One parse-time diagnostic with its position kept as data. Line and
 * col are 1-based; PARSE_DIAG_NO_POSITION (0) means the diagnostic
 * applies to the file as a whole and no position is available. */
#define PARSE_DIAG_NO_POSITION 0
#define PARSE_DIAG_SEVERITY_ERROR "error"

typedef struct parse_diagnostic {
    char *file_name;
    int line;
    int col;
    const char *severity;   /* borrowed: a string literal */
    char *message;
} parse_diagnostic;

/* Builds an engine with the standard security manager. Ports
 * Engine(), which is this(new SecurityManagerImpl()). */
engine *engine_new(void);

/* Builds an engine with the provided security manager and parses
 * lang.aus into it. The manager is in force from the start, so the
 * lang.aus parse already reads it. The engine holds `secman` itself,
 * not a copy. Ports Engine(SecurityManagerInt). */
engine *engine_new_with_security(struct security_manager *secman);

/* Appends a parsed class. Ports Engine.addClass: once engine_new has
 * finished, a static class is instantiated as soon as it is added, so
 * its constructor runs before the parser reaches the next class. A
 * static class whose instantiation fails leaves the failure as the
 * parse exception (engine_parse_exception), where Java throws. */
void engine_add_class(engine *e, ast_class *cls);

/* Loads the include `name` ("math.aus", "acme/net.aus") into this
 * engine now. Ports Engine.addInclude: the engine's modules are tried
 * first, then each include path; an include already loaded is not
 * parsed again. Returns NULL on success, or the reason it was refused
 * where Java throws. Reporting a refusal is the caller's job: the
 * parser stops the parse with it, the include operator returns it as
 * an exception. The parse exception is cleared on entry, so when this
 * returns NULL, engine_parse_exception says whether the parse of the
 * included source stopped with an error Java would throw, which the
 * caller treats the same way. A syntax error inside the included
 * source is reported by that parse, as for any other. */
const char *engine_add_include(engine *e, const char *name);

/* Marks the engine as having seen a parse error. Set by the lexer
 * fallback and by the grammar's report_error path. */
void engine_set_parse_error(engine *e);

/* Clears the sticky parse-error flag. Deliberately does NOT clear the
 * collected diagnostics: callers clear the flag precisely when they are
 * about to report the failure, so dropping diagnostics here would
 * discard them at the moment the consumer needs them. */
void engine_clear_parse_error(engine *e);
bool engine_has_parse_error(const engine *e);

/* --- the error a parse stopped with -------------------------------
 *
 * aussom-base has two kinds of parse error. A syntax error is logged,
 * recorded as a diagnostic and sets the parse-error flag; the parse
 * stops but nothing is thrown. The other errors (an extern function
 * that is not public, a number literal out of range, a duplicate
 * overload, an extern class that is not found or not permitted, an
 * include that fails, a source file that cannot be read) throw
 * aussomException, which stops the parse and reaches whoever started
 * it. Those are not logged, recorded or flagged by the parse.
 *
 * C has no exceptions, so a parse that stops that way returns false
 * and leaves the error here for the caller. Each parse entry point
 * clears it first, so it always describes the most recent parse. */

/* The exception's message, as Java's getMessage() gives it: for an
 * error tied to a source position, "<file> [<line>]: <message>\n";
 * otherwise just the message. NULL when the last parse did not stop
 * this way. */
const char *engine_parse_exception(const engine *e);

/* The message without the file and line in front, followed by the
 * Aussom stack trace when there is one, as Java's getAussomStackTrace()
 * gives it (less its final newline). This is what Java's CLI prints.
 * NULL when engine_parse_exception is NULL. */
const char *engine_parse_exception_stack_trace(const engine *e);

/* Sets and clears the error above. Used by the parser and the parse
 * entry points. Strings are copied. */
void engine_set_parse_exception(engine *e, const char *message,
                                const char *aussom_stack_trace);
void engine_clear_parse_exception(engine *e);

/* Records a diagnostic. Called at the same points that write a parse
 * message to the console. Strings are copied. */
void engine_add_parse_diagnostic(engine *e, const char *file_name, int line,
                                 int col, const char *message);

size_t engine_parse_diagnostic_count(const engine *e);
const parse_diagnostic *engine_get_parse_diagnostic(const engine *e, size_t i);
void engine_clear_parse_diagnostics(engine *e);

/* --- logging ------------------------------------------------------
 *
 * Ports Engine.getLogger / Engine.setLogger. Interpreter internals and
 * the standard library `c` class both route here, so a message can
 * always be traced back to the engine that produced it.
 *
 * The free `console_*` functions in console.h remain for the few
 * bootstrap paths that run before or outside any engine (the CLI's own
 * argument errors, for one). Prefer these whenever an engine is in
 * scope. */

/* Installs this engine's logger. NULL restores the built-in default
 * implementation. The pointer is borrowed: the caller keeps it alive
 * for as long as it is installed. */
void engine_set_logger(engine *e, const console_logger *logger);

/* The logger installed on this engine, or NULL when it is using the
 * built-in default. Unlike console_get_logger this may return NULL,
 * because "no logger of its own" is a distinct state that also selects
 * the engine's own log_level. */
const console_logger *engine_get_logger(const engine *e);

/* Verbosity for the built-in default implementation. Has no effect
 * once a logger has been installed, which owns its own gating. */
void          engine_set_log_level(engine *e, console_level level);
console_level engine_get_log_level(const engine *e);

/* Unprefixed. log is gated at INFO; print/println always emit. */
void engine_log(engine *e, const char *s);
void engine_print(engine *e, const char *s);
void engine_println(engine *e, const char *s);

/* Leveled: prefixed with the priority tag, gated by the engine's level. */
void engine_trc(engine *e, const char *s);
void engine_dbg(engine *e, const char *s);
void engine_info(engine *e, const char *s);
void engine_warn(engine *e, const char *s);
void engine_err(engine *e, const char *s);

/* --- standard library modules (the port of LangRegistry) ----------
 *
 * Every engine starts with the embedded base library. A host adds its
 * own modules per engine, so what one engine can include never depends
 * on what another was given. */

/* Adds or replaces a module on THIS engine. Ports Engine.addModule /
 * LangRegistry.put. `name` is the include name in filesystem form, the
 * same shape engine_get_include returns ("http.aus", "acme/net.aus").
 * Both strings are copied, so a caller may pass transient buffers. */
void engine_add_module(engine *e, const char *name, const char *source);

/* The source for a module, or NULL when this engine has no such
 * module. Ports LangRegistry.get. */
const char *engine_module_source(const engine *e, const char *name);

/* Ports LangRegistry.contains. */
bool engine_has_module(const engine *e, const char *name);

/* Enumerates this engine's modules, so a host can report or audit what
 * an engine is entitled to include. */
size_t      engine_module_count(const engine *e);
const char *engine_module_name(const engine *e, size_t i);

/* --- extern packages (see extern_registry.h) ----------------------
 *
 * Every engine starts with the built-in packages com.aussom.types and
 * com.aussom.stdlib. A host adds its own per engine. */

/* Adds `pkg` to THIS engine. Returns false and changes nothing when a
 * package with that name is already registered, so the built-in
 * packages cannot be replaced and adding the same package twice is
 * harmless; this follows Java, where a class already loaded is never
 * loaded again. Also returns false when `pkg` or its name is NULL.
 * Callable at any time, from any thread. The tables are copied. */
bool engine_add_extern_package(engine *e, const aussom_extern_package *pkg);

/* The class named by a full extern reference, such as
 * "com.aussom.types.AussomBool": the package is everything before the
 * last dot, the class everything after it. NULL when this engine has no
 * such package or the package has no such class. */
const extern_class *engine_find_extern_class(engine *e, const char *ref);

/* This engine's class definition for a primitive type, or NULL when
 * the type has none. Ports Engine.getPrimitiveClassDef.
 *
 * Primitives do not carry a definition -- see av_object_init -- so
 * every dispatch on a primitive resolves it through here. Callers with
 * a value in hand should prefer class_def_for(), which returns the
 * value's own definition when it has one. */
struct ast_class *engine_primitive_class_def(const engine *e, c_type t);

size_t engine_class_count(const engine *e);
ast_class *engine_get_class(const engine *e, size_t i);

/* Looks up a top-level class by name in THIS engine only (linear
 * scan). Returns NULL when absent. Mirrors Engine.getClassByName. */
ast_class *engine_get_class_by_name(const engine *e, const char *name);
bool engine_contains_class(const engine *e, const char *name);

/* Resolves a class by name: this engine first, then the Universe's
 * stdlib classes. Use this for `new`, inheritance, and instanceof so
 * the implicit "object" base and other lang types are visible. */
ast_class *engine_resolve_class(const engine *e, const char *name);

/* Static-class lookup: this engine's own static instances first
 * (from includes / program statics), then the Universe's
 * lang static classes (`c`, `lang`, ...). */
struct av_type *engine_get_static_class(const engine *e, const char *name);
bool engine_contains_static_class(const engine *e, const char *name);

/* Registers a static-class instance under `name` for this engine. */
void engine_register_static(engine *e, const char *name, struct av_type *inst);

/* The includes this engine has loaded, in load order: the include name
 * for a module, the path it was found at for a file. Ports
 * Engine.getIncludes. */
size_t engine_include_count(const engine *e);
const char *engine_get_include(const engine *e, size_t i);

/* Adds a filesystem search path for includes. Ports
 * Engine.addIncludePath, including its behaviour of appending a '/'
 * when the caller omits one.
 *
 * These are consulted by engine_add_include only when the engine's own
 * modules do not carry the name, matching the tier order in Java's
 * Engine.addInclude. */
void engine_add_include_path(engine *e, const char *path);
size_t engine_include_path_count(const engine *e);
const char *engine_get_include_path(const engine *e, size_t i);

/* Adds a path an include may not be loaded from. Ports
 * Engine.addExcludePath, with the same trailing-separator behaviour. */
void engine_add_exclude_path(engine *e, const char *path);
size_t engine_exclude_path_count(const engine *e);
const char *engine_get_exclude_path(const engine *e, size_t i);

/* True when `test_path` starts with any registered exclude path. Ports
 * Engine.isPathExcludePath, which is public there too. */
bool engine_is_path_exclude_path(const engine *e, const char *test_path);

/* Runs a parsed program. Finds the first class declaring a `main`
 * function, instantiates it, and calls main. Mirrors Java
 * Engine.run() / Engine.callMain().
 *
 * `program_name` is not used. It once named a root call frame, but
 * Java's run() starts main under an empty root that adds nothing to a
 * trace, and C now does the same. It may be NULL.
 *
 * Call after parsing. Refreshes
 * limits and registers the calling thread as the interpreter thread
 * for the length of the program body, exactly as the Java version
 * does.
 *
 * Returns main's int return value when it returns one, 0 on success
 * otherwise, and non-zero on failure. Where Java throws
 * aussomException (parse errors present, or no main class found) this
 * logs through the engine logger and returns 1, since C has no
 * exceptions to propagate.
 *
 * main is passed the strings added with engine_add_main_arg and
 * engine_add_main_args, as one list. */
int engine_run(engine *e, const char *program_name);

/* Adds a string to the list passed to main(args). Ports
 * Engine.addMainArg. The string is copied. */
void engine_add_main_arg(engine *e, const char *arg);

/* Adds `count` strings to the list passed to main(args), in order.
 * Ports Engine.addMainArgs. The strings are copied. */
void engine_add_main_args(engine *e, const char *const *args, size_t count);

/* ============================================================
 * Script mode
 *
 * Script mode runs bare top-level statements, the way a REPL, a
 * notebook or a script file without a main class needs. Ports the
 * script-mode API on com.aussom.Engine.
 *
 * Turning it on builds a synthetic class named
 * ENGINE_SCRIPT_CLASS_NAME with an empty main(args), and an instance of
 * it bound to a long-lived environment. Each submission's top-level
 * statements are appended to that main's body and run against that
 * environment, so variables, classes and closures from earlier
 * submissions stay visible to later ones.
 *
 * The synthetic class is never added to the engine's class table, so
 * engine_run and the classic parse path behave the same whether or not
 * script mode is on.
 *
 * Everything here is gated by aussom.script.mode.enable, checked on
 * every entry so revoking the property takes effect at once.
 *
 * Java throws aussomException for a denial, for script mode being off
 * and for a parse error. Here the calls that return a value return an
 * exception value with the same text instead; the others return the
 * text, or NULL on success.
 * ============================================================ */

/* The synthetic script class's name. Ports Engine.SCRIPT_CLASS_NAME. */
#define ENGINE_SCRIPT_CLASS_NAME "__script_main"

/* True while script mode is on. */
bool engine_is_script_mode(const engine *e);

/* Turns script mode on or off. Turning it on builds a fresh synthetic
 * class, instance and environment, so a session that was turned off
 * and on again starts empty. Turning it off keeps them; it only stops
 * further submissions. Setting the mode it already has does nothing.
 * Returns NULL, or the error text when aussom.script.mode.enable is
 * not set or the synthetic class could not be instantiated. */
const char *engine_set_script_mode(engine *e, bool on);

/* The file name script-mode parses report on their nodes, and so in
 * errors and stack traces. Defaults to "<script>". A host running a
 * real file sets it once, after turning script mode on. */
void engine_set_script_file_name(engine *e, const char *file_name);
const char *engine_get_script_file_name(const engine *e);

/* The synthetic script class, or NULL if script mode has never been
 * turned on. Its main's body holds every statement the session has
 * parsed, which is how tooling (an LSP provider, a debugger) walks
 * them. Read-only; triggers no parsing or evaluation. */
struct ast_class *engine_get_script_class(const engine *e);

/* The live environment script mode evaluates top-level statements
 * against, or NULL if script mode has never been turned on. Its locals
 * hold every top-level binding the session has made. Not a copy: a
 * caller that changes it changes the running session's scope.
 * Read-only; triggers no parsing or evaluation. */
struct environment *engine_get_script_environment(const engine *e);

/* Parses `source` and runs only the statements it added. Ports
 * Engine.evalLine(source, lineNumber), which is
 * engine_parse_script_line followed by engine_eval_parsed_script; Java's
 * evalLine(source) is this with line_number 1.
 *
 * line_number is the line the first source line reports as, so a
 * snippet that starts on line 42 of a file keeps its errors on the
 * right lines.
 *
 * Returns the value of the last statement run, or null when there was
 * none. A runtime error comes back as the exception value. A parse
 * error comes back as an exception with the text
 * "Engine.parseScriptLine: parse error."; the parse diagnostics hold
 * the details. */
struct av_type *engine_eval_line(engine *e, const char *source, int line_number);

/* The parse half of engine_eval_line. Appends the statements `source`
 * holds to the script main's body without running them, and sets
 * *body to that body for engine_eval_parsed_script. Splitting the two
 * gives a debugger a place to set breakpoints on the new nodes before
 * they run. Ports Engine.parseScriptLine.
 *
 * On a parse error the statements this call appended are removed, the
 * engine's parse-error flag is cleared (its diagnostics are kept), and
 * the error text is returned. Returns NULL on success. */
struct ast_statement_list;
const char *engine_parse_script_line(engine *e, const char *source, int line_number,
                                     struct ast_statement_list **body);

/* The run half of engine_eval_line. Runs the statements in `body` that
 * no earlier call ran, against the script environment, and stops at
 * the first exception, return or break. Ports
 * Engine.evalParsedScript. `body` must be the one
 * engine_parse_script_line gave back.
 *
 * The statements are marked as run before the first one starts, so a
 * failure part way through never leaves some to run on the next call.
 * Picks up any limit changed in policy since the last call, and
 * registers this thread with the engine while it runs.
 *
 * Returns the last statement's value (the returned value for a
 * return), null when nothing was left to run, or an exception. */
struct av_type *engine_eval_parsed_script(engine *e, struct ast_statement_list *body);

/* ============================================================
 * Control: cancel, pause and resume
 *
 * A running Aussom program is stopped or suspended by asking it to,
 * not by killing or signalling the thread it runs on. The engine holds
 * a control state that the interpreter reads at every checkpoint:
 *
 *   - every loop back edge (while and for)
 *   - every Aussom function call
 *   - every batch of regex subject reads
 *   - every sleep slice
 *
 * RUNNING lets the program continue and costs one read and a compare.
 * PAUSED blocks the interpreter thread at the checkpoint until the
 * state changes. CANCELLED makes the checkpoint hand back an exception
 * whose id is ENGINE_CANCELLED_EXCEPTION_ID.
 *
 * Three properties matter to callers:
 *
 * 1. The result is a value, not a longjmp. Runtime errors already
 *    travel back to the caller as exception values, so cancellation
 *    needs no special unwinding path.
 *
 * 2. The id is distinct. A host running untrusted code on a timeout
 *    must be able to tell "this program ran too long" from "this
 *    program has a bug" -- the two call for completely different
 *    handling.
 *
 * 3. Aussom code cannot catch it. Cancellation is the host's decision,
 *    so a try/catch in the script re-raises it instead of swallowing
 *    it.
 *
 * Cancel outranks pause: a paused program that is cancelled ends
 * without having to be resumed first.
 * ============================================================ */

/* Exception id reported when a program is stopped by engine_cancel. */
#define ENGINE_CANCELLED_EXCEPTION_ID "EXECUTION_CANCELLED"

/* Exception id reported when a call would nest deeper than the
 * engine's call depth limit. */
#define ENGINE_CALL_DEPTH_EXCEEDED_ID "CALL_DEPTH_EXCEEDED"

/* Exception id reported when the interpreter is about to run out of
 * native stack. Port of Engine.STACK_OVERFLOW_ID; see stack_guard.h. */
#define ENGINE_STACK_OVERFLOW_ID "STACK_OVERFLOW"

/* Requests that the running program stop. Safe to call from any thread,
 * including before the program starts. The state is sticky: it stays
 * set until engine_clear_cancel, so a reused engine must be cleared.
 * Wakes any thread parked in a pause. */
void engine_cancel(engine *e);
bool engine_is_cancelled(const engine *e);

/* Clears cancellation so the engine can run again, returning the
 * previous value. Leaves a pause in place. */
bool engine_clear_cancel(engine *e);

/* Asks the running program to stop at its next checkpoint and wait.
 * Ignored when the engine is already cancelled, since cancel outranks
 * pause. Returns immediately: use engine_await_paused to wait until
 * the program has actually stopped. */
void engine_pause(engine *e);

/* Lets a paused program continue. */
void engine_resume(engine *e);

control_state engine_get_control_state(const engine *e);

/* True when a pause has been requested and every thread running this
 * engine's code has reached a checkpoint and parked. False when no
 * thread is running the engine at all, because nothing has stopped. */
bool engine_is_fully_paused(engine *e);

/* Blocks until engine_is_fully_paused would return true, or until
 * timeout_ms elapses. Returns true if the engine reached a full pause.
 * A timeout_ms of 0 polls once without waiting. */
bool engine_await_paused(engine *e, int64_t timeout_ms);

/* The checkpoint itself, called by the interpreter. Returns false to
 * keep going, and true when the caller must stop and raise the
 * cancellation exception. Blocks while the state is PAUSED.
 *
 * Not usually called directly: eval_check_control wraps it and builds
 * the exception. Exposed for the stdlib checkpoints (sleep slices and
 * regex batches) which need the same handshake without an AST node. */
bool engine_await_resume_or_cancel(engine *e);

/* ============================================================
 * Interpreter thread registration and accounting
 * ============================================================ */

/* Registers the calling thread as running this engine's code, for the
 * length of one program body. Returns a scope to hand to
 * engine_exit_interpreter_thread. Records the thread's CPU and
 * allocation counters so the difference can be banked on the way out.
 *
 * Bracket every program body with this:
 *
 *     thread_scope *sc = engine_enter_interpreter_thread(eng);
 *     ... run the program ...
 *     engine_exit_interpreter_thread(sc);
 */
thread_scope *engine_enter_interpreter_thread(engine *e);

/* Ends a registration, banking the thread's CPU and allocation usage.
 * Safe to call with NULL. */
void engine_exit_interpreter_thread(thread_scope *sc);

/* How many threads are currently running this engine's code.
 *
 * The Java engine exposes the thread ids themselves
 * (getInterpreterThreadIds). pthread_t is opaque and not portably an
 * integer, so there is no honest C equivalent of the id list; the
 * count is the part that carries, and it is what a host actually uses
 * -- to tell "nothing is running" from "one program is running" from
 * "several are". */
size_t engine_interpreter_thread_count(engine *e);

/* A small positive integer naming the calling thread, the stand-in for
 * Java's Thread.getId() that a debugger uses to tell its threads apart.
 * As in Java, it is process-wide rather than per engine, and stays the
 * same for the life of the thread. Assigned the first time a thread
 * asks, counting up from 1, and never reused. */
int64_t engine_thread_id(void);

/* CPU nanoseconds this engine has used: banked totals from finished
 * threads plus what its live threads have used so far. */
uint64_t engine_get_cpu_nanos(engine *e);

/* Bytes this engine's threads have allocated. This is a flow counter
 * of everything handed out since the baseline, not a live-set size; it
 * never decreases and says nothing about what is still held. */
uint64_t engine_get_allocated_bytes(engine *e);

/* Zeroes the banked CPU and allocation totals and rebaselines every
 * live thread, so the next read measures from here. */
void engine_reset_accounting(engine *e);

/* Bytes this engine still retains.
 *
 * NOT IMPLEMENTED: always returns ENGINE_FOOTPRINT_UNAVAILABLE.
 *
 * The Java engine answers this by walking the object graph from the
 * engine's roots with an identity map for cycle detection, because a
 * shared JVM heap gives it no other way to attribute bytes to one
 * engine. That walk is deliberately not ported. Under a per-engine
 * allocator the number is a field read instead of a traversal, and it
 * is both cheaper and more accurate than a walk that has to estimate
 * struct sizes and guess about sharing. */
int64_t engine_measure_retained_footprint(engine *e);

/* Bytes this engine's parsed class definitions retain. NOT
 * IMPLEMENTED for the same reason as
 * engine_measure_retained_footprint; always returns
 * ENGINE_FOOTPRINT_UNAVAILABLE. */
int64_t engine_get_class_definition_bytes(engine *e);

/* Returned by the footprint calls to mean "this runtime cannot measure
 * that", which a caller must be able to tell apart from a measured
 * zero. Matches the Java accounting contract, where the counters
 * likewise degrade to -1 rather than lying. */
#define ENGINE_FOOTPRINT_UNAVAILABLE (-1)

/* ============================================================
 * Debugging
 *
 * Ports com.aussom.AussomDebuggingInt, which Engine implements: install
 * a debugger (debugger.h), find the nodes on a source line, set and
 * clear breakpoints, and evaluate source in a paused frame. Step modes,
 * per-thread state and everything a debug adapter shows live in the
 * debugger, not here.
 *
 * With no debugger installed, ast_node_eval pays one branch per node.
 * With one installed, before each node it checks the node's breakpoint
 * flag, then asks should_pause_for_step, and calls on_pause when either
 * says to stop. After each node it reports a new exception value to
 * on_exception.
 * ============================================================ */

/* Installs `d`, or clears it when `d` is NULL. Installing turns debug
 * mode on; clearing turns it off. Ports Engine.setDebugger.
 *
 * Call before any interpreter thread starts. The table may be swapped
 * during a session, but turning debug mode on under a running program
 * is not supported, as in Java.
 *
 * Gated by the security property aussom.debugger.enable. Returns NULL
 * on success, or the reason it was refused where Java throws. Clearing
 * is always allowed. The table is borrowed: the caller keeps it alive
 * for as long as it is installed. */
const char *engine_set_debugger(engine *e, const aussom_debugger *d);

/* The installed debugger, or NULL. Ports Engine.getDebugger. */
const aussom_debugger *engine_get_debugger(const engine *e);

/* True while a debugger is installed. Ports Engine.isDebugMode. */
bool engine_is_debug_mode(const engine *e);

/* Finds every node, across every class this engine has parsed and the
 * script class, whose file name equals `file_name` and whose line is
 * `line`. Ports Engine.findNodesByLine. Nodes are visited in the same
 * order as Java: each class, then its members, then its functions,
 * depth first, with the script class last.
 *
 * Writes up to `max` matches to `out` and returns how many there are in
 * all, so a caller can pass 0 to count them first. Costs one walk of
 * the whole tree: call it per breakpoint request, not on a hot path. */
size_t engine_find_nodes_by_line(engine *e, const char *file_name, int line,
                                 ast_node **out, size_t max);

/* Sets the breakpoint flag on the first node engine_find_nodes_by_line
 * finds, which is usually the outermost statement on the line. Returns
 * false when the line has no node. Ports Engine.setBreakpoint. */
bool engine_set_breakpoint(engine *e, const char *file_name, int line);

/* Clears the breakpoint flag on every node on the line. Returns true if
 * any was set. Ports Engine.clearBreakpoint. */
bool engine_clear_breakpoint(engine *e, const char *file_name, int line);

/* Clears every breakpoint flag in the engine. Returns true if any was
 * set. Ports Engine.clearAllBreakpoints. */
bool engine_clear_all_breakpoints(engine *e);

/* Parses `source` as statements and runs them in `frame`: its `this`,
 * locals and call stack. Ports Engine.evalInFrame. A debugger calls it
 * from on_pause with the environment it was handed.
 *
 * Returns the value of the last statement, or null when there is none.
 * A `return` statement's value is unwrapped. A runtime error is
 * returned as its exception value.
 *
 * Gated by aussom.debugger.enable, checked on every call. A security
 * denial and a parse error are returned as exceptions too, where Java
 * throws: "Engine.evalInFrame: Security exception, ..." and
 * "Engine.evalInFrame: parse error." */
struct av_type *engine_eval_in_frame(engine *e, const char *source,
                                     struct environment *frame);

/* ============================================================
 * Limits
 * ============================================================ */

/* The engine's current limits snapshot. Never NULL. */
const engine_limits *engine_get_limits(const engine *e);

/* Rebuilds the snapshot from the security properties. Called at engine
 * construction and at the start of each program, so a host that
 * rewrites policy between runs is honored. */
void engine_refresh_limits(engine *e);

/* Convenience for the hot path: the call depth limit, 0 for none. */
int64_t engine_get_max_call_depth(const engine *e);

/* The security manager for this engine. Ports
 * Engine.getSecurityManager. Gates read it with the
 * security_manager_get_property_* calls (security_manager.h). */
struct security_manager *engine_get_security_manager(const engine *e);

/* ============================================================
 * Extern class allowlist
 * ============================================================ */

/* Decides whether a script may bind the named extern class with an
 * `extern class` declaration. Consulted by the parser as each
 * declaration is reduced, which is the single point where a class name
 * enters the engine: a class that cannot be declared cannot be
 * constructed or called either.
 *
 * Returns true when aussom.extern.allowlist.enforce is false, which is
 * the default and the historical behavior. When it is true the name
 * must match an entry of aussom.extern.allowed, either exactly or
 * through a `pkg.*` prefix that admits that package and everything
 * under it. An absent or unusable list denies, so switching
 * enforcement on never permits more than intended. */
bool engine_is_extern_class_allowed(engine *e, const char *class_name);

#endif /* AUSSOM_ENGINE_H */
