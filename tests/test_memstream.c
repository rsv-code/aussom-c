/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for the memory stream shim (aussom/memstream.h), which stands
 * in for POSIX open_memstream and fmemopen so the same code builds on
 * Windows. These run on every platform, so they are what catches a
 * divergence between the two implementations.
 */

#include <string.h>

#include "aussom/memstream.h"
#include "greatest.h"

TEST collects_what_is_written(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    ASSERT(fp != NULL);
    fputs("hello", fp);
    fprintf(fp, " %d", 42);
    ASSERT_EQ(0, aussom_memstream_close(fp, &buf, &len));
    ASSERT(buf != NULL);
    ASSERT_STR_EQ("hello 42", buf);
    ASSERT_EQ(strlen("hello 42"), len);
    aussom_memstream_free(buf);
    PASS();
}

/* open_memstream null-terminates even with nothing written, and the
 * Windows path has to match that or callers reading buf as a C string
 * would run off the end. */
TEST empty_stream_is_terminated(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    ASSERT(fp != NULL);
    ASSERT_EQ(0, aussom_memstream_close(fp, &buf, &len));
    ASSERT(buf != NULL);
    ASSERT_EQ(0, (int)len);
    ASSERT_EQ('\0', buf[0]);
    aussom_memstream_free(buf);
    PASS();
}

/* Embedded NUL bytes must survive, because the length is what callers
 * use for binary content even though most of them want a string. */
TEST keeps_embedded_nul(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    ASSERT(fp != NULL);
    fwrite("a\0b", 1, 3, fp);
    ASSERT_EQ(0, aussom_memstream_close(fp, &buf, &len));
    ASSERT_EQ(3, (int)len);
    ASSERT_EQ('a', buf[0]);
    ASSERT_EQ('\0', buf[1]);
    ASSERT_EQ('b', buf[2]);
    aussom_memstream_free(buf);
    PASS();
}

TEST reads_back_a_buffer(void)
{
    const char *src = "line one\nline two\n";
    FILE *fp = aussom_memstream_open_read(src, strlen(src));
    ASSERT(fp != NULL);
    char got[64] = {0};
    size_t n = fread(got, 1, sizeof(got) - 1, fp);
    fclose(fp);
    ASSERT_EQ(strlen(src), n);
    ASSERT_STR_EQ(src, got);
    PASS();
}

/* The parser hands this an empty stdlib entry in some configurations,
 * so it must not be treated as failure. */
TEST reads_back_empty(void)
{
    FILE *fp = aussom_memstream_open_read("", 0);
    ASSERT(fp != NULL);
    char got[8];
    ASSERT_EQ(0, (int)fread(got, 1, sizeof(got), fp));
    fclose(fp);
    PASS();
}

TEST free_tolerates_null(void)
{
    aussom_memstream_free(NULL);
    PASS();
}

SUITE(memstream)
{
    RUN_TEST(collects_what_is_written);
    RUN_TEST(empty_stream_is_terminated);
    RUN_TEST(keeps_embedded_nul);
    RUN_TEST(reads_back_a_buffer);
    RUN_TEST(reads_back_empty);
    RUN_TEST(free_tolerates_null);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(memstream);
    GREATEST_MAIN_END();
}
