/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * See aussom/memstream.h.
 */

#include "aussom/memstream.h"

#include <stdlib.h>

#if defined(_WIN32)

/* Windows has no memory-backed FILE, so the bytes go to a temporary
 * file and are read back at close. tmpfile() is used rather than a
 * named path because it needs no name, is opened in binary update mode,
 * and is removed by the C library when it is closed, which leaves
 * nothing behind if the process dies mid-write.
 *
 * The cost is real but lands only on the string-building paths
 * (toString, JSON, string format, stack traces) on the one platform
 * that cannot do better. */

FILE *aussom_memstream_open(char **bufp, size_t *sizep)
{
    (void)bufp; (void)sizep;
    return tmpfile();
}

FILE *aussom_memstream_open_read(const void *data, size_t len)
{
    FILE *fp = tmpfile();
    if (fp == NULL) return NULL;
    if (len > 0 && fwrite(data, 1, len, fp) != len) { fclose(fp); return NULL; }
    rewind(fp);
    return fp;
}

int aussom_memstream_close(FILE *fp, char **bufp, size_t *sizep)
{
    *bufp = NULL;
    *sizep = 0;
    if (fp == NULL) return -1;

    if (fflush(fp) != 0) { fclose(fp); return -1; }

    long end = ftell(fp);
    if (end < 0) { fclose(fp); return -1; }
    rewind(fp);

    char *buf = (char *)malloc((size_t)end + 1);
    if (buf == NULL) { fclose(fp); return -1; }

    size_t got = (end > 0) ? fread(buf, 1, (size_t)end, fp) : 0;
    buf[got] = '\0';        /* open_memstream always null-terminates */
    fclose(fp);

    *bufp = buf;
    *sizep = got;
    return 0;
}

#else

/* Everywhere else this is exactly open_memstream, which already
 * publishes the buffer into the two out-parameters when the
 * stream is closed. */

FILE *aussom_memstream_open(char **bufp, size_t *sizep)
{
    return open_memstream(bufp, sizep);
}

FILE *aussom_memstream_open_read(const void *data, size_t len)
{
    return fmemopen((void *)data, len, "r");
}

int aussom_memstream_close(FILE *fp, char **bufp, size_t *sizep)
{
    (void)bufp; (void)sizep;
    if (fp == NULL) return -1;
    return fclose(fp);
}

#endif

void aussom_memstream_free(char *buf)
{
    free(buf);
}
