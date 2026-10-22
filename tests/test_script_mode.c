/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for script mode: engine_set_script_mode, engine_eval_line and
 * its two halves, engine_parse_script_line and
 * engine_eval_parsed_script. Ports aussom-base's
 * src/test/java/ScriptMode.java, numbered the same way, plus the
 * script-mode cases of Closures.java and ParseDiagnostics.java.
 */

#include <stdio.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/ast.h"
#include "aussom/av.h"
#include "aussom/console.h"
#include "aussom/debugger.h"
#include "aussom/engine.h"
#include "aussom/environment.h"
#include "aussom/parser.h"
#include "aussom/security_manager.h"
#include "greatest.h"

/* CapturingLogger: everything but trc and dbg, so a case can check
 * what the script printed. */
static char captured[16384];
static size_t captured_len;
static void cap(const char *s)
{
    size_t n = strlen(s);
    if (captured_len + n >= sizeof(captured)) n = sizeof(captured) - 1 - captured_len;
    memcpy(captured + captured_len, s, n);
    captured_len += n;
    captured[captured_len] = '\0';
}
static void cap_line(const char *s)
{
    cap(s);
    cap("\n");
}
static void ignore(const char *s) { (void)s; }
static const console_logger capturing = {
    cap_line, ignore, ignore, cap_line, cap_line, cap_line, cap, cap_line,
};

static void clear_out(void)
{
    captured[0] = '\0';
    captured_len = 0;
}

/* Engine(new TestSecurityManagerImpl()), which permits script mode. */
static engine *new_script_engine(void)
{
    engine *e = engine_new_with_security(test_security_manager_impl_new());
    engine_set_logger(e, &capturing);
    clear_out();
    return e;
}

/* The same, already in script mode. */
static engine *in_script_mode(const char *file_name)
{
    engine *e = new_script_engine();
    if (engine_set_script_mode(e, true) != NULL) return NULL;
    if (file_name != NULL) engine_set_script_file_name(e, file_name);
    return e;
}

static bool is_parse_error(av_type *r)
{
    return av_is_ex(r)
        && strcmp(av_exception_get_text((av_exception *)r),
                  "Engine.parseScriptLine: parse error.") == 0;
}

static ast_statement_list *script_body(engine *e)
{
    size_t n;
    ast_funct_def **fl = ast_class_get_functions_by_name(engine_get_script_class(e),
                                                         "main", &n);
    return n == 1 ? ast_funct_def_get_instruction_list(fl[0]) : NULL;
}

static int64_t int_of(av_type *v)
{
    return (v != NULL && v->type == C_TYPE_INT) ? av_int_get((av_int *)v) : -999;
}

/* The one diagnostic whose message holds `fragment`, or NULL when there
 * is not exactly one. */
static const parse_diagnostic *diag_with(engine *e, const char *fragment)
{
    const parse_diagnostic *found = NULL;
    for (size_t i = 0; i < engine_parse_diagnostic_count(e); i++) {
        const parse_diagnostic *d = engine_get_parse_diagnostic(e, i);
        if (strstr(d->message, fragment) != NULL) {
            if (found != NULL) return NULL;
            found = d;
        }
    }
    return found;
}

/* --- NORMAL mode unchanged --------------------------------------- */

TEST off_by_default(void)
{
    engine *e = engine_new_with_security(default_security_manager_impl_new());
    engine_set_logger(e, &capturing);
    ASSERT_FALSE(aussom_parse_string(e, "x = 5;", "<t>"));
    ASSERT(engine_has_parse_error(e));
    PASS();
}

TEST eval_line_requires_script_mode(void)
{
    engine *e = new_script_engine();
    av_type *r = engine_eval_line(e, "x = 5;", 1);
    ASSERT(av_is_ex(r));
    ASSERT(strstr(av_exception_get_text((av_exception *)r),
                  "script mode is not enabled") != NULL);
    PASS();
}

SUITE(normal_mode)
{
    RUN_TEST(off_by_default);
    RUN_TEST(eval_line_requires_script_mode);
}

/* --- basic evalLine flow ----------------------------------------- */

TEST single_statement(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "c.log(\"hi\");", 1);
    ASSERT(strstr(captured, "hi") != NULL);
    PASS();
}

TEST persistent_locals(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "x = 5;", 1);
    clear_out();
    engine_eval_line(e, "c.log(x);", 1);
    ASSERT(strstr(captured, "5") != NULL);
    PASS();
}

TEST class_declared_via_eval_line(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "class point { public x = 0; public y = 0; "
        "public point(int X, int Y) { this.x = X; this.y = Y; } "
        "public sum() { return this.x + this.y; } }", 1);
    clear_out();
    engine_eval_line(e, "p = new point(3, 4); c.log(p.sum());", 1);
    ASSERT(strstr(captured, "7") != NULL);
    PASS();
}

TEST top_level_function_rejected(void)
{
    engine *e = in_script_mode(NULL);
    ASSERT(is_parse_error(engine_eval_line(e, "public f(x) { return x * 2; }", 1)));
    PASS();
}

SUITE(basic_eval)
{
    RUN_TEST(single_statement);
    RUN_TEST(persistent_locals);
    RUN_TEST(class_declared_via_eval_line);
    RUN_TEST(top_level_function_rejected);
}

/* --- error recovery ---------------------------------------------- */

TEST parse_error_recovery(void)
{
    engine *e = in_script_mode(NULL);
    ASSERT(is_parse_error(engine_eval_line(e, "x = ;", 1)));
    engine_eval_line(e, "x = 5;", 1);
    clear_out();
    engine_eval_line(e, "c.log(x);", 1);
    ASSERT(strstr(captured, "5") != NULL);
    PASS();
}

TEST runtime_error_preserves_locals(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "y = 7;", 1);
    ASSERT(av_is_ex(engine_eval_line(e, "z = 1/0;", 1)));
    clear_out();
    engine_eval_line(e, "c.log(y);", 1);
    ASSERT(strstr(captured, "7") != NULL);
    PASS();
}

SUITE(error_recovery)
{
    RUN_TEST(parse_error_recovery);
    RUN_TEST(runtime_error_preserves_locals);
}

/* --- classical pipeline independence ----------------------------- */

TEST run_ignores_script_mode(void)
{
    engine *e = new_script_engine();
    ASSERT(aussom_parse_string(e,
        "class foo { public main(args) { c.log(\"foo-main\"); } }", "user.aus"));
    ASSERT_EQ(NULL, engine_set_script_mode(e, true));
    engine_eval_line(e, "xx = 99;", 1);
    clear_out();
    ASSERT_EQ(0, engine_run(e, "user.aus"));
    ASSERT(strstr(captured, "foo-main") != NULL);
    ASSERT(strstr(captured, "99") == NULL);
    PASS();
}

TEST script_mode_after_run(void)
{
    engine *e = new_script_engine();
    ASSERT(aussom_parse_string(e,
        "class bar { public main(args) { c.log(\"bar-main\"); } }", "user.aus"));
    engine_run(e, "user.aus");
    ASSERT_EQ(NULL, engine_set_script_mode(e, true));
    clear_out();
    engine_eval_line(e, "c.log(\"after-run\");", 1);
    ASSERT(strstr(captured, "after-run") != NULL);
    PASS();
}

SUITE(independence)
{
    RUN_TEST(run_ignores_script_mode);
    RUN_TEST(script_mode_after_run);
}

/* --- file name and line offset ----------------------------------- */

TEST script_file_name_default(void)
{
    engine *e = in_script_mode(NULL);
    ASSERT_STR_EQ("<script>", engine_get_script_file_name(e));
    av_type *r = engine_eval_line(e, "z = 1/0;", 1);
    ASSERT(av_is_ex(r));
    ASSERT_EQ(1, av_exception_get_line((av_exception *)r));
    PASS();
}

TEST set_script_file_name_propagates(void)
{
    engine *e = in_script_mode("testScript.aus");
    av_type *r = engine_eval_line(e, "1/0;", 1);
    ASSERT(av_is_ex(r));
    ASSERT_EQ(1, av_exception_get_line((av_exception *)r));
    ast_statement_list *body = script_body(e);
    ASSERT(body != NULL && body->statements.count >= 1);
    ast_node *last = body->statements.items[body->statements.count - 1];
    ASSERT_STR_EQ("testScript.aus", last->file_name);
    PASS();
}

TEST single_line_offset(void)
{
    engine *e = in_script_mode("testScript.aus");
    av_type *r = engine_eval_line(e, "1/0;", 42);
    ASSERT(av_is_ex(r));
    ASSERT_EQ(42, av_exception_get_line((av_exception *)r));
    PASS();
}

TEST multi_line_offset(void)
{
    engine *e = in_script_mode("testScript.aus");
    av_type *r = engine_eval_line(e, "a = 1;\nb = 2;\n1/0;", 10);
    ASSERT(av_is_ex(r));
    ASSERT_EQ(12, av_exception_get_line((av_exception *)r));
    PASS();
}

SUITE(file_name_and_offset)
{
    RUN_TEST(script_file_name_default);
    RUN_TEST(set_script_file_name_propagates);
    RUN_TEST(single_line_offset);
    RUN_TEST(multi_line_offset);
}

/* --- security gating --------------------------------------------- */

TEST denied_by_default(void)
{
    engine *e = engine_new_with_security(default_security_manager_impl_new());
    const char *err = engine_set_script_mode(e, true);
    ASSERT(err != NULL && strstr(err, "aussom.script.mode.enable") != NULL);
    ASSERT_FALSE(engine_is_script_mode(e));
    PASS();
}

TEST denied_with_base_manager(void)
{
    engine *e = engine_new_with_security(security_manager_impl_new());
    ASSERT(engine_set_script_mode(e, true) != NULL);
    PASS();
}

TEST allowed_with_test_manager(void)
{
    engine *e = in_script_mode(NULL);
    ASSERT(engine_is_script_mode(e));
    engine_eval_line(e, "c.log(\"ok\");", 1);
    ASSERT(strstr(captured, "ok") != NULL);
    PASS();
}

SUITE(security)
{
    RUN_TEST(denied_by_default);
    RUN_TEST(denied_with_base_manager);
    RUN_TEST(allowed_with_test_manager);
}

/* --- engine_get_script_class ------------------------------------- */

TEST null_before_enable(void)
{
    engine *e = new_script_engine();
    ASSERT_EQ(NULL, engine_get_script_class(e));
    ASSERT_EQ(NULL, engine_get_script_environment(e));
    PASS();
}

TEST has_appended_statements(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "x = 5; y = 7;", 1);
    ast_class *cls = engine_get_script_class(e);
    ASSERT(cls != NULL);
    ASSERT_STR_EQ(ENGINE_SCRIPT_CLASS_NAME, cls->base.name);
    ast_statement_list *body = script_body(e);
    ASSERT(body != NULL);
    ASSERT_EQ(2, (int)body->statements.count);
    PASS();
}

TEST not_in_class_registry(void)
{
    engine *e = in_script_mode(NULL);
    ASSERT_EQ(NULL, engine_get_class_by_name(e, ENGINE_SCRIPT_CLASS_NAME));
    PASS();
}

SUITE(get_script_class)
{
    RUN_TEST(null_before_enable);
    RUN_TEST(has_appended_statements);
    RUN_TEST(not_in_class_registry);
}

/* --- mixed-source fixtures --------------------------------------- */

TEST mixed_source(void)
{
    static const char src[] =
        "include sys;\n"
        "include math;\n"
        "\n"
        "/*\n"
        " * Banner comment block at the top level.\n"
        " */\n"
        "\n"
        "/**\n"
        " * A point in 2D space.\n"
        " */\n"
        "@Test(name = \"Point tests\")\n"
        "class point {\n"
        "    public x = 0;\n"
        "    public y = 0;\n"
        "\n"
        "    // Single-line comment inside a class.\n"
        "    public point(int X, int Y) {\n"
        "        this.x = X;\n"
        "        this.y = Y;\n"
        "    }\n"
        "\n"
        "    /**\n"
        "     * Distance from origin.\n"
        "     * @r The Euclidean distance as a double.\n"
        "     */\n"
        "    public dist() {\n"
        "        return math.sqrt(this.x * this.x + this.y * this.y);\n"
        "    }\n"
        "}\n"
        "\n"
        "// Top-level statements that exercise the class.\n"
        "p = new point(3, 4);\n"
        "c.log(\"dist = \" + p.dist());\n";

    engine *e = in_script_mode("mixed.aus");
    engine_eval_line(e, src, 1);
    ASSERT_FALSE(engine_has_parse_error(e));
    ASSERT(strstr(captured, "dist = 5") != NULL);

    ast_class *point = engine_get_class_by_name(e, "point");
    ASSERT(point != NULL);
    ASSERT(point->base.doc_node != NULL);
    ASSERT_EQ(1, (int)point->base.annotations.count);
    ast_annotation *ann = (ast_annotation *)point->base.annotations.items[0];
    ASSERT_STR_EQ("Test", ast_annotation_get_name(ann));
    ASSERT_STR_EQ("Point tests", ast_annotation_find_value(ann, "name"));

    ast_funct_def *dist = ast_class_get_funct(point, "dist", "");
    ASSERT(dist != NULL);
    ASSERT(dist->base.doc_node != NULL);

    ASSERT_EQ(2, (int)script_body(e)->statements.count);
    PASS();
}

TEST top_level_control_flow(void)
{
    static const char src[] =
        "include sys;\n"
        "\n"
        "/**\n"
        " * Simple stateful counter.\n"
        " */\n"
        "class counter {\n"
        "    public value = 0;\n"
        "    public inc() { this.value += 1; return this; }\n"
        "    public get() { return this.value; }\n"
        "}\n"
        "\n"
        "// for loop at top level using the class.\n"
        "ctr = new counter();\n"
        "for (i = 0; i < 5; i = i + 1) {\n"
        "    ctr.inc();\n"
        "}\n"
        "\n"
        "// if/else at top level.\n"
        "if (ctr.get() == 5) {\n"
        "    c.log(\"counter reached \" + ctr.get());\n"
        "} else {\n"
        "    c.log(\"counter did not reach 5\");\n"
        "}\n"
        "\n"
        "// while loop at top level.\n"
        "sum = 0;\n"
        "n = 1;\n"
        "while (n <= 10) {\n"
        "    sum = sum + n;\n"
        "    n = n + 1;\n"
        "}\n"
        "c.log(\"sum = \" + sum);\n"
        "\n"
        "// try/catch at top level.\n"
        "try {\n"
        "    bad = 1 / 0;\n"
        "    c.log(\"should not reach\");\n"
        "} catch (e) {\n"
        "    c.log(\"caught div-by-zero\");\n"
        "}\n";

    engine *e = in_script_mode("ctrlflow.aus");
    av_type *r = engine_eval_line(e, src, 1);
    ASSERT_FALSE(engine_has_parse_error(e));
    ASSERT(engine_get_class_by_name(e, "counter") != NULL);

    /* ctr=, for, if/else, sum=, n=, while, log, try/catch. */
    ASSERT_EQ(8, (int)script_body(e)->statements.count);

    char *reached = strstr(captured, "counter reached 5");
    char *sum = strstr(captured, "sum = 55");
    char *caught = strstr(captured, "caught div-by-zero");
    ASSERT(reached != NULL);
    ASSERT(sum != NULL && sum > reached);
    ASSERT(caught != NULL && caught > sum);
    ASSERT(r != NULL);
    ASSERT_FALSE(av_is_ex(r));
    PASS();
}

SUITE(fixtures)
{
    RUN_TEST(mixed_source);
    RUN_TEST(top_level_control_flow);
}

/* --- split parse and eval ---------------------------------------- */

TEST parse_then_eval_runs_exactly_once(void)
{
    engine *e = in_script_mode(NULL);
    ast_statement_list *body = NULL;
    ASSERT_EQ(NULL, engine_parse_script_line(e, "x = 5;", 1, &body));
    ASSERT_EQ(1, (int)body->statements.count);
    engine_eval_parsed_script(e, body);
    clear_out();
    engine_eval_line(e, "c.log(x);", 1);
    ASSERT(strstr(captured, "5") != NULL);
    PASS();
}

TEST cursor_advance_does_not_re_eval(void)
{
    engine *e = in_script_mode(NULL);
    ast_statement_list *body = NULL;
    ASSERT_EQ(NULL, engine_parse_script_line(e, "ctr = 1;", 1, &body));
    engine_eval_parsed_script(e, body);
    ASSERT_EQ(NULL, engine_parse_script_line(e, "ctr = ctr + 10;", 2, &body));
    engine_eval_parsed_script(e, body);
    clear_out();
    engine_eval_line(e, "c.log(ctr);", 1);
    ASSERT(strstr(captured, "11") != NULL);
    PASS();
}

static bool fired;
static bool bp_on_pause(void *data, ast_node *node, environment *env,
                        aussom_pause_reason reason)
{
    (void)data; (void)node; (void)env; (void)reason;
    fired = true;
    return true;
}
static bool bp_no_step(void *data, ast_node *node, environment *env)
{
    (void)data; (void)node; (void)env;
    return false;
}
static bool bp_on_exception(void *data, av_exception *ex, environment *env)
{
    (void)data; (void)ex; (void)env;
    return true;
}
static const aussom_debugger bp_debugger = {
    NULL, bp_on_pause, bp_no_step, bp_on_exception,
};

TEST breakpoint_between_parse_and_eval(void)
{
    engine *e = new_script_engine();
    fired = false;
    ASSERT_EQ(NULL, engine_set_debugger(e, &bp_debugger));
    ASSERT_EQ(NULL, engine_set_script_mode(e, true));
    engine_set_script_file_name(e, "dap.aus");

    /* The user-source nodes do not exist before this. */
    ast_statement_list *body = NULL;
    ASSERT_EQ(NULL, engine_parse_script_line(e, "y = 42;", 1, &body));
    ASSERT(engine_set_breakpoint(e, "dap.aus", 1));
    engine_eval_parsed_script(e, body);
    ASSERT(fired);
    PASS();
}

TEST parse_error_rollback(void)
{
    engine *e = in_script_mode(NULL);
    ast_statement_list *body = NULL;
    engine_parse_script_line(e, "a = 1;", 1, &body);
    engine_parse_script_line(e, "b = 2;", 2, &body);
    size_t before = body->statements.count;
    const char *err = engine_parse_script_line(e, "c = ;", 3, NULL);
    ASSERT(err != NULL && strstr(err, "parse error") != NULL);
    ASSERT_EQ(before, body->statements.count);
    PASS();
}

TEST parse_requires_script_mode(void)
{
    engine *e = new_script_engine();
    const char *err = engine_parse_script_line(e, "x = 5;", 1, NULL);
    ASSERT(err != NULL && strstr(err, "script mode is not enabled") != NULL);
    PASS();
}

TEST eval_requires_script_mode(void)
{
    engine *e = in_script_mode(NULL);
    ast_statement_list *body = NULL;
    ASSERT_EQ(NULL, engine_parse_script_line(e, "z = 9;", 1, &body));
    ASSERT_EQ(NULL, engine_set_script_mode(e, false));
    av_type *r = engine_eval_parsed_script(e, body);
    ASSERT(av_is_ex(r));
    ASSERT(strstr(av_exception_get_text((av_exception *)r),
                  "script mode is not enabled") != NULL);
    PASS();
}

TEST parse_denied_by_default_security(void)
{
    engine *e = engine_new_with_security(default_security_manager_impl_new());
    const char *err = engine_parse_script_line(e, "x = 1;", 1, NULL);
    ASSERT(err != NULL && strstr(err, "script mode is not enabled") != NULL);
    PASS();
}

SUITE(split_parse_eval)
{
    RUN_TEST(parse_then_eval_runs_exactly_once);
    RUN_TEST(cursor_advance_does_not_re_eval);
    RUN_TEST(breakpoint_between_parse_and_eval);
    RUN_TEST(parse_error_rollback);
    RUN_TEST(parse_requires_script_mode);
    RUN_TEST(eval_requires_script_mode);
    RUN_TEST(parse_denied_by_default_security);
}

/* --- closures (Closures.java) ------------------------------------ */

TEST top_level_closure(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "n = 5;", 1);
    engine_eval_line(e, "f = ::smScale(int x) { return x * n; };", 1);
    ASSERT_EQ(15, int_of(engine_eval_line(e, "return f.call(3);", 1)));
    PASS();
}

TEST closure_in_script_declared_class(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "class smOwner { public smOwner() {} "
        "public make(int n) { return ::smAdd(int x) { return x + n; }; } }", 1);
    ASSERT_EQ(15, int_of(engine_eval_line(e,
        "o = new smOwner(); a = o.make(10); return a.call(5);", 1)));
    PASS();
}

TEST duplicate_closure_name(void)
{
    engine *e = in_script_mode(NULL);
    ASSERT(is_parse_error(engine_eval_line(e,
        "class dupOwner { public dupOwner() {} "
        "public a() { f = ::dupClo(int x) { return x; }; } "
        "public b() { g = ::dupClo(int y) { return y; }; } }", 1)));
    PASS();
}

TEST closure_overloads_accepted(void)
{
    engine *e = in_script_mode(NULL);
    engine_eval_line(e, "class ovOwner { public ovOwner() {} "
        "public a() { f = ::ovClo(int x) { return x; }; return f.call(4); } "
        "public b() { g = ::ovClo(string s) { return s; }; return g.call(\"hi\"); } }", 1);
    av_type *r = engine_eval_line(e, "o = new ovOwner(); return \"\" + o.a() + o.b();", 1);
    ASSERT(r->type == C_TYPE_STRING);
    ASSERT_STR_EQ("4hi", av_string_get((av_string *)r));
    PASS();
}

/* C-only: the hoist's own duplicate check, for two top-level closures
 * with one name and signature. */
TEST duplicate_top_level_closure(void)
{
    engine *e = in_script_mode("dup.aus");
    engine_eval_line(e, "f = ::twice(int x) { return x; };", 1);
    ASSERT(is_parse_error(engine_eval_line(e, "g = ::twice(int y) { return y; };", 2)));
    const parse_diagnostic *d = diag_with(e, "Duplicate method overload 'twice(i)'");
    ASSERT(d != NULL);
    ASSERT_EQ(2, d->line);
    PASS();
}

SUITE(closures)
{
    RUN_TEST(top_level_closure);
    RUN_TEST(closure_in_script_declared_class);
    RUN_TEST(duplicate_closure_name);
    RUN_TEST(closure_overloads_accepted);
    RUN_TEST(duplicate_top_level_closure);
}

/* --- parse diagnostics (ParseDiagnostics.java) ------------------- */

TEST lexer_error_honors_offset(void)
{
    engine *e = in_script_mode("cell-1");
    ASSERT(is_parse_error(engine_eval_line(e, "s = $;", 20)));
    const parse_diagnostic *lex = diag_with(e, "Illegal character");
    ASSERT(lex != NULL);
    ASSERT_EQ(20, lex->line);
    PASS();
}

TEST lexer_and_parser_agree(void)
{
    engine *e = in_script_mode("cell-1");
    ASSERT(is_parse_error(engine_eval_line(e, "s = $;", 20)));
    ASSERT(engine_parse_diagnostic_count(e) >= 2);
    for (size_t i = 0; i < engine_parse_diagnostic_count(e); i++) {
        ASSERT_EQ(20, engine_get_parse_diagnostic(e, i)->line);
    }
    PASS();
}

TEST multi_line_diagnostic_offset(void)
{
    engine *e = in_script_mode("cell-1");
    ASSERT(is_parse_error(engine_eval_line(e, "a = 1;\nb = 2;\nc = $;", 10)));
    const parse_diagnostic *lex = diag_with(e, "Illegal character");
    ASSERT(lex != NULL);
    ASSERT_EQ(12, lex->line);
    PASS();
}

TEST classic_path_unaffected(void)
{
    engine *e = new_script_engine();
    ASSERT_FALSE(aussom_parse_string(e, "class c { public c() { x = $; } }", "<t>"));
    ASSERT(engine_has_parse_error(e));
    const parse_diagnostic *lex = diag_with(e, "Illegal character");
    ASSERT(lex != NULL);
    ASSERT_EQ(1, lex->line);
    PASS();
}

TEST clean_submission_has_no_diagnostics(void)
{
    engine *e = in_script_mode("cell-1");
    engine_eval_line(e, "x = 5;", 1);
    ASSERT_EQ(0, (int)engine_parse_diagnostic_count(e));
    PASS();
}

TEST survives_internal_clear_parse_error(void)
{
    engine *e = in_script_mode("cell-1");
    ASSERT(is_parse_error(engine_eval_line(e, "z = ;", 7)));
    ASSERT(engine_parse_diagnostic_count(e) > 0);
    ASSERT_FALSE(engine_has_parse_error(e));
    PASS();
}

TEST script_submissions_do_not_accumulate(void)
{
    engine *e = in_script_mode("cell-1");
    ASSERT(is_parse_error(engine_eval_line(e, "z = ;", 5)));
    ASSERT(engine_parse_diagnostic_count(e) > 0);
    engine_eval_line(e, "y = 2;", 6);
    ASSERT_EQ(0, (int)engine_parse_diagnostic_count(e));
    PASS();
}

/* OrphanProbe: aussom_parse_statements with script mode never turned
 * on, so a top-level closure has no script class to land on. */
TEST orphan_closure_has_position(void)
{
    engine *e = new_script_engine();
    ASSERT_FALSE(aussom_parse_statements(e, "<t>",
        "a = 1;\nf = ::orphan() { return 1; };", 0, ast_statement_list_new()));
    ASSERT(engine_has_parse_error(e));
    const parse_diagnostic *d = diag_with(e, "Closure defined outside a class");
    ASSERT(d != NULL);
    ASSERT_STR_EQ("<t>", d->file_name);
    ASSERT_EQ(2, d->line);
    PASS();
}

SUITE(parse_diagnostics)
{
    RUN_TEST(lexer_error_honors_offset);
    RUN_TEST(lexer_and_parser_agree);
    RUN_TEST(multi_line_diagnostic_offset);
    RUN_TEST(classic_path_unaffected);
    RUN_TEST(clean_submission_has_no_diagnostics);
    RUN_TEST(survives_internal_clear_parse_error);
    RUN_TEST(script_submissions_do_not_accumulate);
    RUN_TEST(orphan_closure_has_position);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    aussom_gc_init();

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(normal_mode);
    RUN_SUITE(basic_eval);
    RUN_SUITE(error_recovery);
    RUN_SUITE(independence);
    RUN_SUITE(file_name_and_offset);
    RUN_SUITE(security);
    RUN_SUITE(get_script_class);
    RUN_SUITE(fixtures);
    RUN_SUITE(split_parse_eval);
    RUN_SUITE(closures);
    RUN_SUITE(parse_diagnostics);
    GREATEST_MAIN_END();
}
