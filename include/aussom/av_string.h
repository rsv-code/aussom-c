/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_STRING_H
#define AUSSOM_AV_STRING_H

#include <stdint.h>
#include <stdio.h>

#include "aussom/av_object.h"

/* Mirrors com.aussom.types.AussomString (cString).
 *
 * The value is a sequence of UTF-16 code units with a length, as a
 * java.lang.String is. It may hold U+0000 and unpaired surrogates, and
 * every index the language reports (length, charAt, substr, indexOf,
 * regex positions) counts these units.
 *
 * Each string also carries a NUL-terminated `char *` form for C code
 * that needs one: names, messages, map keys and the like. That form is
 * lossless. It is UTF-8, except that U+0000 is written as the bytes
 * C0 80 and an unpaired surrogate as its own 3-byte encoding (ED A0 80
 * for U+D800), the way Java's "modified UTF-8" does it. Neither pattern
 * occurs in valid UTF-8, so text without those characters is plain
 * UTF-8 and the form converts back to the same units. Text leaving the
 * interpreter goes through av_text_write_out, which turns it into real
 * UTF-8.
 *
 * All buffers are GC-allocated copies. A string is never changed after
 * it is shared, except by av_string_set and av_string_set_units, which
 * replace both forms. */
typedef struct av_string {
    av_object base;
    uint16_t *units;   /* UTF-16 code units */
    size_t len;        /* number of units */
    char *text;        /* the lossless char form, NUL-terminated */
    size_t text_len;   /* bytes in text, not counting the NUL */
} av_string;

/* A string from the lossless char form (plain UTF-8 is a valid input).
 * Bytes that are not valid UTF-8 become U+FFFD. */
av_string *av_string_new(const char *value);

/* As av_string_new, for `nbytes` bytes that need not end in a NUL. */
av_string *av_string_new_len(const char *value, size_t nbytes);

/* A string of `n` UTF-16 code units. */
av_string *av_string_new_units(const uint16_t *units, size_t n);

/* The lossless char form. */
const char *av_string_get(const av_string *s);

/* The UTF-16 code units, and their count in *n (n may be NULL). */
const uint16_t *av_string_units(const av_string *s, size_t *n);

/* The number of UTF-16 code units (Java's String.length()). */
size_t av_string_length(const av_string *s);

void av_string_set(av_string *s, const char *value);
void av_string_set_units(av_string *s, const uint16_t *units, size_t n);

/* String.compareTo: the difference of the first differing UTF-16
 * units, else the difference of the lengths. */
int64_t av_string_compare(const av_string *a, const av_string *b);

/* The same comparison for two strings in the lossless char form. */
int64_t av_text_compare(const char *a, const char *b);

/* --- conversions between units and the lossless char form ------- */

/* Decodes `nbytes` of the lossless char form into GC-allocated units,
 * writing their count to *n. */
uint16_t *av_text_to_units(const char *text, size_t nbytes, size_t *n);

/* Encodes units as the lossless char form (GC, NUL-terminated),
 * writing its byte count to *nbytes (nbytes may be NULL). */
char *av_units_to_text(const uint16_t *units, size_t n, size_t *nbytes);

/* Writes the lossless char form `text` to `out` as real UTF-8: C0 80
 * becomes a zero byte and an unpaired surrogate becomes '?', which is
 * what Java's output streams write for them. Use it wherever text
 * leaves the interpreter (the console, stdout, stderr). */
void av_text_write_out(const char *text, FILE *out);

/* The number of UTF-16 code units the lossless char form `text`
 * decodes to. */
size_t av_utf8_count(const char *text);

#endif /* AUSSOM_AV_STRING_H */
