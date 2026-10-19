/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for the installable logger (aussom/console.h), the C port of
 * com.aussom.LoggingInt and Engine.setLogger. A host whose stdout goes
 * nowhere, such as an Android application, installs its own here, so
 * what matters is that every entry point routes through the installed
 * logger and that the default comes back.
 */

#include <string.h>

#include "aussom/console.h"
#include "greatest.h"

/* Records which method fired and with what, so each entry point can be
 * checked for routing to its own slot rather than to any slot. */
static char g_last_method[16];
static char g_last_text[128];
static int  g_calls;

#define RECORDER(name)                                        \
    static void rec_##name(const char *s)                     \
    {                                                         \
        snprintf(g_last_method, sizeof g_last_method, "%s", #name); \
        snprintf(g_last_text, sizeof g_last_text, "%s", s);   \
        g_calls++;                                            \
    }
RECORDER(log) RECORDER(trc) RECORDER(dbg) RECORDER(info)
RECORDER(warn) RECORDER(err) RECORDER(print) RECORDER(println)

static const console_logger recorder = {
    rec_log, rec_trc, rec_dbg, rec_info,
    rec_warn, rec_err, rec_print, rec_println,
};

static void reset(void)
{
    g_last_method[0] = '\0';
    g_last_text[0] = '\0';
    g_calls = 0;
}

TEST default_logger_is_installed(void)
{
    console_set_logger(NULL);
    ASSERT(console_get_logger() != NULL);
    ASSERT_EQ(console_default_logger(), console_get_logger());
    PASS();
}

TEST every_entry_point_routes_to_its_own_slot(void)
{
    console_set_logger(&recorder);

    struct { void (*fn)(const char *); const char *name; } cases[] = {
        { console_log, "log" },     { console_trc, "trc" },
        { console_dbg, "dbg" },     { console_info, "info" },
        { console_warn, "warn" },   { console_err, "err" },
        { console_print, "print" }, { console_println, "println" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset();
        cases[i].fn("payload");
        ASSERT_EQ(1, g_calls);
        ASSERT_STR_EQ(cases[i].name, g_last_method);
        ASSERT_STR_EQ("payload", g_last_text);
    }

    console_set_logger(NULL);
    PASS();
}

/* A replacement logger owns its own gating, exactly as in Java where
 * setLevel lives on DefaultLoggingImpl and not on LoggingInt. So a
 * level that would suppress the default must not suppress this one. */
TEST installed_logger_is_not_gated_by_the_default_level(void)
{
    console_level saved = console_get_level();
    console_set_logger(&recorder);
    console_set_level(CONSOLE_ERROR);   /* would suppress trace */

    reset();
    console_trc("still delivered");
    ASSERT_EQ(1, g_calls);
    ASSERT_STR_EQ("still delivered", g_last_text);

    console_set_level(saved);
    console_set_logger(NULL);
    PASS();
}

TEST setting_null_restores_the_default(void)
{
    console_set_logger(&recorder);
    ASSERT_EQ(&recorder, console_get_logger());
    console_set_logger(NULL);
    ASSERT_EQ(console_default_logger(), console_get_logger());

    /* And the recorder must genuinely be out of the path. */
    reset();
    console_set_level(CONSOLE_ERROR);   /* keep the default quiet */
    console_trc("goes to the default");
    ASSERT_EQ(0, g_calls);
    console_set_level(CONSOLE_INFO);
    PASS();
}

TEST level_round_trips(void)
{
    console_level saved = console_get_level();
    console_set_level(CONSOLE_WARNING);
    ASSERT_EQ(CONSOLE_WARNING, console_get_level());
    console_set_level(saved);
    ASSERT_EQ(saved, console_get_level());
    PASS();
}

SUITE(console)
{
    RUN_TEST(default_logger_is_installed);
    RUN_TEST(every_entry_point_routes_to_its_own_slot);
    RUN_TEST(installed_logger_is_not_gated_by_the_default_level);
    RUN_TEST(setting_null_restores_the_default);
    RUN_TEST(level_round_trips);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(console);
    GREATEST_MAIN_END();
}
