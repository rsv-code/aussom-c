/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for the per-engine extern package registry
 * (engine_add_extern_package, extern_registry.h): what a host registers
 * is visible only to that engine, a reference must name a registered
 * package and class, and a package name, once registered, is never
 * replaced.
 */

#include <stdio.h>

#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/console.h"
#include "aussom/engine.h"
#include "aussom/environment.h"
#include "aussom/parser.h"
#include "greatest.h"

/* Several cases fail on purpose, and a failure logs through the
 * engine's logger. Swallow that so it does not look like test output. */
static void quiet(const char *s) { (void)s; }
static const console_logger silent = {
    quiet, quiet, quiet, quiet, quiet, quiet, quiet, quiet,
};

static av_type *calc_one(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env; (void)args;
    return &av_int_new(1)->base.base;
}

static av_type *calc_two(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env; (void)args;
    return &av_int_new(2)->base.base;
}

static const aussom_extern_method calc_one_methods[] = {
    { "value", calc_one },
    { NULL, NULL }
};

static const aussom_extern_method calc_two_methods[] = {
    { "value", calc_two },
    { NULL, NULL }
};

static const aussom_extern_class pkg_one_classes[] = {
    { "Calc", calc_one_methods },
    { NULL, NULL }
};

static const aussom_extern_class pkg_two_classes[] = {
    { "Calc", calc_two_methods },
    { NULL, NULL }
};

/* Two classes in one package, for the inheritance cases. */
static const aussom_extern_class pkg_pair_classes[] = {
    { "Calc", calc_one_methods },
    { "Other", calc_two_methods },
    { NULL, NULL }
};
static const aussom_extern_package pkg_pair = { "test.pair", pkg_pair_classes };

/* The same package name backed by different code. */
static const aussom_extern_package pkg_one = { "test.pkg", pkg_one_classes };
static const aussom_extern_package pkg_two = { "test.pkg", pkg_two_classes };

/* Returns what Calc.value() returns. */
static const char *call_value =
    "static extern class calc : test.pkg.Calc {\n"
    "    public extern value();\n"
    "}\n"
    "class Main { public main() { return calc.value(); } }\n";

static engine *quiet_engine(void)
{
    engine *e = engine_new();
    engine_set_logger(e, &silent);
    return e;
}

/* Parses `source`, then runs it when it parsed cleanly. Returns what
 * main returns, or -1 when the parse failed. */
static int parse_and_run(engine *e, const char *source)
{
    if (!aussom_parse_string(e, source, "<test>")) return -1;
    return engine_run(e, "<test>");
}

TEST engines_bind_only_their_own_package(void)
{
    engine *a = quiet_engine();
    engine *b = quiet_engine();
    ASSERT(engine_add_extern_package(a, &pkg_one));
    ASSERT(engine_add_extern_package(b, &pkg_two));

    ASSERT_EQ(1, parse_and_run(a, call_value));
    ASSERT_EQ(2, parse_and_run(b, call_value));
    PASS();
}

TEST an_unregistered_package_is_a_parse_error(void)
{
    engine *e = quiet_engine();
    ASSERT_EQ(-1, parse_and_run(e, call_value));
    PASS();
}

TEST an_unregistered_class_is_a_parse_error(void)
{
    engine *e = quiet_engine();
    ASSERT(engine_add_extern_package(e, &pkg_one));
    ASSERT_EQ(-1, parse_and_run(e,
        "static extern class other : test.pkg.Other { }\n"
        "class Main { public main() { return 0; } }\n"));
    PASS();
}

TEST a_reference_without_a_package_is_a_parse_error(void)
{
    engine *e = quiet_engine();
    ASSERT(engine_add_extern_package(e, &pkg_one));
    ASSERT_EQ(-1, parse_and_run(e,
        "static extern class calc : Calc { }\n"
        "class Main { public main() { return 0; } }\n"));
    PASS();
}

TEST a_missing_method_throws_extern_no_such_method(void)
{
    engine *e = quiet_engine();
    ASSERT(engine_add_extern_package(e, &pkg_one));
    ASSERT_EQ(5, parse_and_run(e,
        "static extern class calc : test.pkg.Calc {\n"
        "    public extern missing();\n"
        "}\n"
        "class Main {\n"
        "    public main() {\n"
        "        try { calc.missing(); }\n"
        "        catch (ex) { if (ex.getId() == \"EXTERN_NO_SUCH_METHOD\") { return 5; } }\n"
        "        return 0;\n"
        "    }\n"
        "}\n"));
    PASS();
}

TEST a_second_package_of_the_same_name_is_ignored(void)
{
    engine *e = quiet_engine();
    ASSERT(engine_add_extern_package(e, &pkg_one));
    ASSERT_FALSE(engine_add_extern_package(e, &pkg_two));
    ASSERT_EQ(1, parse_and_run(e, call_value));
    PASS();
}

TEST a_built_in_package_cannot_be_replaced(void)
{
    static const aussom_extern_package fake_types = {
        "com.aussom.types", pkg_one_classes
    };
    engine *e = quiet_engine();
    ASSERT_FALSE(engine_add_extern_package(e, &fake_types));
    ASSERT(engine_find_extern_class(e, "com.aussom.types.AussomString") != NULL);
    ASSERT_EQ(NULL, engine_find_extern_class(e, "com.aussom.types.Calc"));
    ASSERT_EQ(3, parse_and_run(e,
        "class Main { public main() { return \"abc\".length(); } }\n"));
    PASS();
}

TEST a_package_without_a_name_is_refused(void)
{
    static const aussom_extern_package unnamed = { NULL, pkg_one_classes };
    engine *e = quiet_engine();
    ASSERT_FALSE(engine_add_extern_package(e, NULL));
    ASSERT_FALSE(engine_add_extern_package(e, &unnamed));
    PASS();
}

/* The caller's tables need not outlive the call. */
TEST a_package_is_copied(void)
{
    aussom_extern_method methods[] = { { "value", calc_one }, { NULL, NULL } };
    aussom_extern_class classes[] = { { "Calc", methods }, { NULL, NULL } };
    char name[] = "test.pkg";
    aussom_extern_package pkg = { name, classes };

    engine *e = quiet_engine();
    ASSERT(engine_add_extern_package(e, &pkg));
    methods[0].fn = calc_two;
    name[0] = 'x';
    ASSERT_EQ(1, parse_and_run(e, call_value));
    PASS();
}

/* --- a static constructor registering a package ------------------
 *
 * The CLI module template's pattern, with app.loadJar replaced by an
 * extern method that registers a package directly: a static class
 * loads the package, and an extern class below it in the same file
 * binds to it. Works because Engine.addClass instantiates the static
 * class before the parser reaches the next class. */

static av_type *loader_load(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)args;
    engine_add_extern_package(environment_engine(env), &pkg_one);
    return &av_bool_new(true)->base.base;
}

static const aussom_extern_method loader_methods[] = {
    { "load", loader_load },
    { NULL, NULL }
};
static const aussom_extern_class loader_classes[] = {
    { "Loader", loader_methods },
    { NULL, NULL }
};
static const aussom_extern_package loader_pkg = { "test.loader", loader_classes };

TEST a_static_constructor_can_load_a_package_for_a_later_class(void)
{
    engine *e = quiet_engine();
    ASSERT(engine_add_extern_package(e, &loader_pkg));
    ASSERT_EQ(1, parse_and_run(e,
        "static extern class loader : test.loader.Loader {\n"
        "    public extern load();\n"
        "}\n"
        "static class boot { public boot() { loader.load(); } }\n"
        "static extern class calc : test.pkg.Calc {\n"
        "    public extern value();\n"
        "}\n"
        "class Main { public main() { return calc.value(); } }\n"));
    PASS();
}

/* --- one extern class per inheritance (astClass.initSync) -------- */

/* Instantiates `decl`'s class `target` and returns 1 when that throws
 * the two-extern-classes error, 0 when it succeeds, 2 otherwise. As in
 * Java, `new` reports it as INSTANTIATION_FAILURE with the message at
 * the head of the stack trace. */
static int instantiate(const char *decls, const char *target)
{
    char src[2048];
    snprintf(src, sizeof(src),
        "%s\n"
        "class Main {\n"
        "    public main() {\n"
        "        try { o = new %s(); }\n"
        "        catch (ex) {\n"
        "            if (ex.getId() == \"INSTANTIATION_FAILURE\" &&\n"
        "                ex.getStackTrace().contains(\"two external classes\")) { return 1; }\n"
        "            return 2;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", decls, target);
    engine *e = quiet_engine();
    engine_add_extern_package(e, &pkg_pair);
    return parse_and_run(e, src);
}

static const char *two_externs =
    "extern class calc : test.pair.Calc { public calc() { } }\n"
    "extern class other : test.pair.Other { public other() { } }\n";

TEST an_extern_class_with_an_extern_parent_is_refused(void)
{
    char decls[1024];
    snprintf(decls, sizeof(decls), "%s%s", two_externs,
        "extern class child : test.pair.Other, calc { public child() { } }\n");
    ASSERT_EQ(1, instantiate(decls, "child"));
    PASS();
}

TEST a_class_with_two_extern_parents_is_refused(void)
{
    char decls[1024];
    snprintf(decls, sizeof(decls), "%s%s", two_externs,
        "class both : calc, other { public both() { } }\n");
    ASSERT_EQ(1, instantiate(decls, "both"));
    PASS();
}

TEST one_extern_backing_is_accepted(void)
{
    char decls[1024];
    snprintf(decls, sizeof(decls), "%s%s", two_externs,
        "class plain { public plain() { } }\n"
        "class sub : calc { public sub() { } }\n"
        "extern class mixed : test.pair.Other, plain { public mixed() { } }\n");
    ASSERT_EQ(0, instantiate(decls, "sub"));
    ASSERT_EQ(0, instantiate(decls, "mixed"));
    PASS();
}

SUITE(extern_packages)
{
    RUN_TEST(engines_bind_only_their_own_package);
    RUN_TEST(an_unregistered_package_is_a_parse_error);
    RUN_TEST(an_unregistered_class_is_a_parse_error);
    RUN_TEST(a_reference_without_a_package_is_a_parse_error);
    RUN_TEST(a_missing_method_throws_extern_no_such_method);
    RUN_TEST(a_second_package_of_the_same_name_is_ignored);
    RUN_TEST(a_built_in_package_cannot_be_replaced);
    RUN_TEST(a_package_without_a_name_is_refused);
    RUN_TEST(a_package_is_copied);
}

SUITE(extern_loading)
{
    RUN_TEST(a_static_constructor_can_load_a_package_for_a_later_class);
}

SUITE(extern_inheritance)
{
    RUN_TEST(an_extern_class_with_an_extern_parent_is_refused);
    RUN_TEST(a_class_with_two_extern_parents_is_refused);
    RUN_TEST(one_extern_backing_is_accepted);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    /* Must run on this thread before any engine exists. */
    aussom_gc_init();

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(extern_packages);
    RUN_SUITE(extern_loading);
    RUN_SUITE(extern_inheritance);
    GREATEST_MAIN_END();
}
