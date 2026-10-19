/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * test_harness.c -- proves the C test wiring works.
 *
 * This file exercises no interpreter code. It exists so a failure here
 * means the harness itself is broken rather than the code under test,
 * and it is the template for the other test files: define tests with
 * TEST, group them with SUITE, list the suites in main.
 *
 * These are C unit tests, which are a different thing from the Aussom
 * language regression suite in tests/interpreter.aus (a copy of
 * aussom-base's) and the aussom-c tests in tests/c-interpreter.aus.
 * Those run through the interpreter's own test runner:
 *
 *     ./build/aussomc -t tests/interpreter.aus
 *     ./build/aussomc -t tests/c-interpreter.aus
 */

#include "greatest.h"

TEST harness_reports_pass(void)
{
    ASSERT_EQ(4, 2 + 2);
    PASS();
}

TEST harness_compares_strings(void)
{
    ASSERT_STR_EQ("aussom", "aussom");
    PASS();
}

SUITE(harness)
{
    RUN_TEST(harness_reports_pass);
    RUN_TEST(harness_compares_strings);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(harness);
    GREATEST_MAIN_END();
}
