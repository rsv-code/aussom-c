/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_STDLIB_CHARSET_H
#define AUSSOM_STDLIB_CHARSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The charsets Buffer's string methods support, converted the way
 * Java's new String(bytes, charset) and String.getBytes(charset) do
 * it: malformed or unmapped input decodes to U+FFFD, and a character
 * a charset can't hold encodes to its replacement ('?' for the byte
 * charsets, U+FFFD for UTF-16 and UTF-32).
 *
 * Supported: UTF-8, US-ASCII, ISO-8859-1, UTF-16, UTF-16BE, UTF-16LE,
 * UTF-32, UTF-32BE, UTF-32LE, windows-1252, and ISO-8859-2 to -9, -11,
 * -13, -15 and -16, each under the names and aliases Java's
 * Charset.forName accepts. The single-byte tables were generated from
 * the JDK 23 charsets. */

typedef struct cs_charset cs_charset;

/* The name ABuffer passes to Java: charSet.toUpperCase().replace("_",
 * "-"). GC-allocated. */
char *cs_java_name(const char *name);

/* Charset.checkName: false for a name Charset.forName rejects with
 * IllegalCharsetNameException (empty, a first character that is not a
 * letter or digit, or any character other than a letter, digit, '-',
 * '+', ':', '_' or '.'). */
bool cs_name_legal(const char *name);

/* The charset for `name` (compared without regard to case), or NULL
 * when it is not supported. */
const cs_charset *cs_lookup(const char *name);

/* new String(bytes, charset): the decoded UTF-16 units (GC), with
 * their count in *n. */
uint16_t *cs_decode_units(const cs_charset *cs, const unsigned char *bytes, size_t len, size_t *n);

/* The same, as the lossless char form strings use (see av_string.h). */
char *cs_decode(const cs_charset *cs, const unsigned char *bytes, size_t len);

/* str.getBytes(charset) of `n` UTF-16 units. Returns GC bytes and
 * writes their count to *out_len. */
unsigned char *cs_encode_units(const cs_charset *cs, const uint16_t *units, size_t n, size_t *out_len);

#endif /* AUSSOM_STDLIB_CHARSET_H */
