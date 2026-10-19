/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_STDLIB_DATE_PATTERN_H
#define AUSSOM_STDLIB_DATE_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Date.format and Date.parse patterns, as java.time.format.
 * DateTimeFormatter reads them in ADate: the pattern letters, quoted
 * text, optional sections and padding of DateTimeFormatterBuilder.
 * appendPattern, printed at UTC in the en_US locale, and parsed
 * strictly with Java's SMART resolver. The rules were learned from the
 * JDK 23 runtime's results (see date_pattern.c). No JDK source was
 * used. */

typedef enum {
    DP_OK,            /* success */
    DP_PARSE_ERROR,   /* a DateTimeParseException; ADate catches it */
    DP_THROWN         /* any other exception, "java.lang.X: text" */
} dp_result;

/* DateTimeFormatter.ofPattern(pat).withZone(UTC).format(instant). The
 * text goes to *out and *nout as UTF-16 units; on failure *msg is the
 * exception. */
dp_result dp_format(const uint16_t *pat, size_t np, int64_t secs, int32_t nanos,
                    uint16_t **out, size_t *nout, const char **msg);

/* ADate.parse: appendPattern(pat), zone UTC, read by
 * ZonedDateTime.parse. Hour, minute and second default to 0 only when
 * the text parsed without them has no hour (no hour of day and no hour
 * of am/pm). The instant goes to *secs and *nanos; on failure *msg is
 * the exception's message. */
dp_result dp_parse(const uint16_t *text, size_t nt, const uint16_t *pat, size_t np,
                   int64_t *secs, int32_t *nanos, const char **msg);

/* Proleptic Gregorian calendar helpers, shared with date_methods.c. */
int64_t date_floordiv(int64_t a, int64_t b);
int64_t date_floormod(int64_t a, int64_t b);
int64_t date_days_from_civil(int64_t y, int m, int d);
void date_civil_from_days(int64_t z, int64_t *y, int *m, int *d);
bool date_is_leap(int64_t y);
int date_days_in_month(int64_t y, int m);

#endif /* AUSSOM_STDLIB_DATE_PATTERN_H */
