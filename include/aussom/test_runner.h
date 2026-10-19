/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unit-test runner. Ports com.aussom.UnitTestRunner: discovers classes
 * with @Test methods, runs each through the per-test hook loop
 * (@Before / @BeforeEach / test / @OnTestFail / @AfterEach / @After),
 * and reports pass/skip/fail counts.
 */

#ifndef AUSSOM_TEST_RUNNER_H
#define AUSSOM_TEST_RUNNER_H

#include <stdbool.h>

#include "aussom/ast_class.h"
#include "aussom/engine.h"

typedef struct test_result {
    int total;
    int passed;
    int skipped;
    int failed;
} test_result;

/* True when `cls` declares at least one @Test method. */
bool test_class_contains_tests(ast_class *cls);

/* Runs every @Test method in `cls` through the full hook loop, using a
 * single cached class instance. Prints per-test results. */
test_result test_runner_run_class(engine *eng, ast_class *cls);

/* Runs every @Test-containing class in `eng`, prints a summary, and
 * returns the aggregate result. Mirrors UnitTestRunner.loadAllTestClasses
 * (the `--test-all` path). */
test_result test_runner_run_all(engine *eng);

/* Runs every @Test-containing class defined in `path`, prints a summary,
 * and returns the aggregate result. Mirrors
 * UnitTestRunner.loadTestClasses(file) (the `--test` path): classes are
 * matched by an exact file-name comparison against `path`. */
test_result test_runner_run_file(engine *eng, const char *path);

/* The `testRunner` (com.aussom.stdlib.ATestRunner) extern methods. */
extern const aussom_extern_method stdlib_atest_runner_methods[];

/* The `test` class's C methods (ATest). */
extern const aussom_extern_method stdlib_atest_methods[];

#endif /* AUSSOM_TEST_RUNNER_H */
