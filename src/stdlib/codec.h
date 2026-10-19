/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Internal byte codecs (hex + base64), rolled in-house for clean
 * provenance. All outputs are GC-allocated. The base64 alphabet and
 * padding match Apache Commons Codec Base64.encodeBase64 (standard,
 * unchunked); hex matches Hex.encodeHexString (lowercase).
 */

#ifndef AUSSOM_STDLIB_CODEC_H
#define AUSSOM_STDLIB_CODEC_H

#include <stddef.h>

/* Lowercase hex string of `len` bytes (GC, null-terminated). */
char *codec_hex_encode(const unsigned char *data, size_t len);

/* Decodes a hex string into a freshly GC-allocated byte array. On
 * success returns the bytes and writes the length to *out_len. Returns
 * NULL on odd length or any non-hex character. */
unsigned char *codec_hex_decode(const char *str, size_t *out_len);

/* codec_hex_decode, and on failure writes commons-codec's
 * Hex.decodeHex message into err ("Odd number of characters 3." or
 * "Illegal hexadecimal character 0x7A at index 0."). */
unsigned char *codec_hex_decode_msg(const char *str, size_t *out_len,
                                    char *err, size_t errsz);

/* new String(bytes, UTF_8): the bytes as a NUL-terminated string,
 * with malformed UTF-8 replaced by U+FFFD as Java's decoder does it
 * (charset.c). */
char *codec_utf8_string(const unsigned char *data, size_t len);

/* String.getBytes(UTF_8) of `str`, a string in the lossless char form
 * (see av_string.h). Returns GC bytes; their count goes to *out_len. */
unsigned char *codec_utf8_bytes(const char *str, size_t *out_len);

/* Standard base64 (with '=' padding, no line breaks) of `len` bytes
 * (GC, null-terminated). */
char *codec_base64_encode(const unsigned char *data, size_t len);

/* Decodes base64 into a freshly GC-allocated byte array, as
 * commons-codec's Base64.decodeBase64 does: characters outside the
 * alphabet are skipped, '-' and '_' count as '+' and '/', decoding
 * stops at the first '=', and leftover bits short of a byte are
 * dropped. It never fails. Writes the decoded length to *out_len. */
unsigned char *codec_base64_decode(const char *str, size_t *out_len);

/* Computes the SHA-1 digest (20 bytes) of `data` into `out`. */
void codec_sha1(const unsigned char *data, size_t len, unsigned char out[20]);

#endif /* AUSSOM_STDLIB_CODEC_H */
