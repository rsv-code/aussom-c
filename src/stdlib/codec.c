/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Hex and base64 byte codecs. Self-authored (clean provenance per the
 * project's dependency policy: the popular single-file base64/hex libs
 * are public domain, which we avoid).
 */

#include "codec.h"

#include "charset.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av_string.h"

static const char HEX[] = "0123456789abcdef";

char *codec_hex_encode(const unsigned char *data, size_t len)
{
    char *out = AUSSOM_ALLOC_ATOMIC(len * 2 + 1);
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = HEX[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = HEX[data[i] & 0xf];
    }
    out[len * 2] = '\0';
    return out;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

unsigned char *codec_hex_decode(const char *str, size_t *out_len)
{
    size_t slen = 0;
    while (str[slen] != '\0') slen++;
    if (slen % 2 != 0) return NULL;

    size_t n = slen / 2;
    unsigned char *out = AUSSOM_ALLOC_ATOMIC(n > 0 ? n : 1);
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(str[i * 2]);
        int lo = hex_val(str[i * 2 + 1]);
        if (hi < 0 || lo < 0) return NULL;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    *out_len = n;
    return out;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *codec_base64_encode(const unsigned char *data, size_t len)
{
    size_t olen = ((len + 2) / 3) * 4;
    char *out = AUSSOM_ALLOC_ATOMIC(olen + 1);
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out[o++] = B64[(n >> 18) & 0x3f];
        out[o++] = B64[(n >> 12) & 0x3f];
        out[o++] = B64[(n >> 6) & 0x3f];
        out[o++] = B64[n & 0x3f];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        unsigned int n = data[i] << 16;
        out[o++] = B64[(n >> 18) & 0x3f];
        out[o++] = B64[(n >> 12) & 0x3f];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8);
        out[o++] = B64[(n >> 18) & 0x3f];
        out[o++] = B64[(n >> 12) & 0x3f];
        out[o++] = B64[(n >> 6) & 0x3f];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

unsigned char *codec_base64_decode(const char *str, size_t *out_len)
{
    size_t slen = strlen(str);
    unsigned char *out = AUSSOM_ALLOC_ATOMIC(slen / 4 * 3 + 3);
    unsigned int acc = 0;
    int count = 0;
    size_t o = 0;
    for (const char *p = str; *p != '\0'; p++) {
        if (*p == '=') break;
        int v = (*p == '-') ? 62 : (*p == '_') ? 63 : b64_val(*p);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned int)v;
        if (++count == 4) {
            out[o++] = (unsigned char)(acc >> 16);
            out[o++] = (unsigned char)(acc >> 8);
            out[o++] = (unsigned char)acc;
            acc = 0;
            count = 0;
        }
    }
    if (count == 2) {
        out[o++] = (unsigned char)(acc >> 4);
    } else if (count == 3) {
        acc >>= 2;
        out[o++] = (unsigned char)(acc >> 8);
        out[o++] = (unsigned char)acc;
    }
    *out_len = o;
    return out;
}

unsigned char *codec_hex_decode_msg(const char *str, size_t *out_len,
                                    char *err, size_t errsz)
{
    /* Hex.decodeHex reads the string's UTF-16 units: it counts them,
     * and reports a bad one by its index and its low byte. */
    size_t n;
    uint16_t *u = av_text_to_units(str, strlen(str), &n);
    if (n % 2 != 0) {
        snprintf(err, errsz, "Odd number of characters %zu.", n);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (u[i] > 0x7F || hex_val((char)u[i]) < 0) {
            snprintf(err, errsz, "Illegal hexadecimal character 0x%02X at index %zu.",
                     (unsigned)(u[i] & 0xFF), i);
            return NULL;
        }
    }
    return codec_hex_decode(str, out_len);
}

/* String.getBytes(UTF_8) of a string in the lossless char form: U+0000
 * is a zero byte and an unpaired surrogate is '?'. */
unsigned char *codec_utf8_bytes(const char *str, size_t *out_len)
{
    size_t n;
    uint16_t *u = av_text_to_units(str, strlen(str), &n);
    return cs_encode_units(cs_lookup("UTF-8"), u, n, out_len);
}

char *codec_utf8_string(const unsigned char *d, size_t len)
{
    return cs_decode(cs_lookup("UTF-8"), d, len);
}

/* --- SHA-1 (FIPS 180-1), self-authored --------------------------- */

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void codec_sha1(const unsigned char *data, size_t len, unsigned char out[20])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    /* Padded message length: append 0x80, zeros, then 64-bit bit count,
     * to a multiple of 64 bytes. */
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *msg = AUSSOM_ALLOC_ATOMIC(total);
    memcpy(msg, data, len);
    memset(msg + len, 0, total - len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++) msg[total - 1 - i] = (unsigned char)((bits >> (8 * i)) & 0xff);

    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)msg[off + i * 4] << 24 | (uint32_t)msg[off + i * 4 + 1] << 16 |
                   (uint32_t)msg[off + i * 4 + 2] << 8 | (uint32_t)msg[off + i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);        k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                 k = 0xCA62C1D6; }
            uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    uint32_t hh[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)((hh[i] >> 24) & 0xff);
        out[i * 4 + 1] = (unsigned char)((hh[i] >> 16) & 0xff);
        out[i * 4 + 2] = (unsigned char)((hh[i] >> 8) & 0xff);
        out[i * 4 + 3] = (unsigned char)(hh[i] & 0xff);
    }
}
