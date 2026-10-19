/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * A stream that collects what is written to it in memory.
 *
 * This has no counterpart in aussom-base, where the same code uses a
 * StringBuilder. It exists because the C port builds those strings with
 * stdio calls into a POSIX open_memstream, and Windows has no
 * equivalent: MinGW provides neither open_memstream nor a way to back a
 * FILE with memory (no fopencookie, no funopen). The close step is a
 * function here rather than plain fclose because the Windows
 * implementation has to do work at that point.
 *
 * Usage, on every platform:
 *
 *     char *buf = NULL; size_t len = 0;
 *     FILE *fp = aussom_memstream_open(&buf, &len);
 *     if (fp == NULL) { ... }
 *     fprintf(fp, ...);
 *     aussom_memstream_close(fp, &buf, &len);
 *     ... use buf ...
 *     aussom_memstream_free(buf);
 *
 * The buffer belongs to the C library, not to the collector, so it must
 * be released with aussom_memstream_free and never handed to GC code.
 * Copy anything that needs to outlive it with AUSSOM_STRDUP first.
 */

#ifndef AUSSOM_MEMSTREAM_H
#define AUSSOM_MEMSTREAM_H

#include <stdio.h>
#include <stddef.h>

/* Opens a memory-collecting stream. Returns NULL on failure, in which
 * case the out-parameters are untouched. */
FILE *aussom_memstream_open(char **bufp, size_t *sizep);

/* Closes the stream and publishes the collected bytes through its
 * out-parameters. Returns 0 on success. On failure the buffer is set to NULL.
 * Must be used instead of fclose on a stream from
 * aussom_memstream_open. */
int aussom_memstream_close(FILE *fp, char **bufp, size_t *sizep);

/* Releases a buffer published by aussom_memstream_close. Safe on NULL. */
void aussom_memstream_free(char *buf);

/* Opens a read-only stream over `len` bytes at `data`, so a caller that
 * needs a FILE can read from memory. This is the read direction of the
 * same gap: POSIX has fmemopen and Windows has nothing equivalent. The
 * bytes are consumed before the call returns, so `data` need not
 * outlive it.
 *
 * Close the result with plain fclose. Returns NULL on failure. */
FILE *aussom_memstream_open_read(const void *data, size_t len);

#endif /* AUSSOM_MEMSTREAM_H */
