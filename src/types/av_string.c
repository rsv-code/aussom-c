/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_string.h"

#include <string.h>

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

static int is_high(uint32_t u) { return u >= 0xD800 && u <= 0xDBFF; }
static int is_low(uint32_t u)  { return u >= 0xDC00 && u <= 0xDFFF; }

/* --- conversions ------------------------------------------------- */

uint16_t *av_text_to_units(const char *text, size_t nbytes, size_t *n)
{
    const unsigned char *d = (const unsigned char *)text;
    /* Never more units than bytes. */
    uint16_t *u = AUSSOM_ALLOC_ATOMIC((nbytes + 1) * sizeof(uint16_t));
    size_t o = 0, i = 0;
    while (i < nbytes) {
        unsigned char c = d[i];
        if (c < 0x80) { u[o++] = c; i++; continue; }
        /* C0 80 is U+0000. */
        if (c == 0xC0 && i + 1 < nbytes && d[i + 1] == 0x80) { u[o++] = 0; i += 2; continue; }
        size_t need;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) lo = 0xA0;
            /* ED A0-BF xx is an unpaired surrogate, which this form
             * keeps, so ED takes the full continuation range. */
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            u[o++] = 0xFFFD; i++; continue;
        }
        size_t k = 1;
        for (; k <= need && i + k < nbytes; k++) {
            unsigned char b = d[i + k];
            unsigned char l = (k == 1) ? lo : 0x80, h = (k == 1) ? hi : 0xBF;
            if (b < l || b > h) break;
        }
        if (k != need + 1) {
            /* Not valid: one U+FFFD for the longest valid start. */
            u[o++] = 0xFFFD;
            i += k;
            continue;
        }
        uint32_t cp = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
        for (size_t j = 1; j <= need; j++) cp = (cp << 6) | (d[i + j] & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u[o++] = (uint16_t)(0xD800 + (cp >> 10));
            u[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u[o++] = (uint16_t)cp;
        }
        i += need + 1;
    }
    *n = o;
    return u;
}

char *av_units_to_text(const uint16_t *units, size_t n, size_t *nbytes)
{
    char *s = AUSSOM_ALLOC_ATOMIC(n * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = units[i];
        if (is_high(cp) && i + 1 < n && is_low(units[i + 1])) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        }
        if (cp == 0) {
            s[o++] = (char)0xC0;
            s[o++] = (char)0x80;
        } else if (cp < 0x80) {
            s[o++] = (char)cp;
        } else if (cp < 0x800) {
            s[o++] = (char)(0xC0 | (cp >> 6));
            s[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            /* An unpaired surrogate lands here too, as ED xx xx. */
            s[o++] = (char)(0xE0 | (cp >> 12));
            s[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            s[o++] = (char)(0xF0 | (cp >> 18));
            s[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    s[o] = '\0';
    if (nbytes != NULL) *nbytes = o;
    return s;
}

void av_text_write_out(const char *text, FILE *out)
{
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *run = p;
    while (*p != '\0') {
        if (p[0] == 0xC0 && p[1] == 0x80) {
            fwrite(run, 1, (size_t)(p - run), out);
            fputc('\0', out);
            p += 2;
            run = p;
        } else if (p[0] == 0xED && p[1] >= 0xA0 && p[1] <= 0xBF && p[2] >= 0x80 && p[2] <= 0xBF) {
            fwrite(run, 1, (size_t)(p - run), out);
            fputc('?', out);
            p += 3;
            run = p;
        } else {
            p++;
        }
    }
    fwrite(run, 1, (size_t)(p - run), out);
}

size_t av_utf8_count(const char *text)
{
    size_t n;
    av_text_to_units(text, strlen(text), &n);
    return n;
}

static int64_t units_compare(const uint16_t *a, size_t na, const uint16_t *b, size_t nb)
{
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int64_t)a[i] - (int64_t)b[i];
    }
    return (int64_t)na - (int64_t)nb;
}

int64_t av_string_compare(const av_string *a, const av_string *b)
{
    return units_compare(a->units, a->len, b->units, b->len);
}

int64_t av_text_compare(const char *a, const char *b)
{
    size_t na, nb;
    uint16_t *ua = av_text_to_units(a, strlen(a), &na);
    uint16_t *ub = av_text_to_units(b, strlen(b), &nb);
    return units_compare(ua, na, ub, nb);
}

/* --- the string type --------------------------------------------- */

static void set_text(av_string *s, const char *value, size_t nbytes)
{
    s->units = av_text_to_units(value, nbytes, &s->len);
    /* Re-encode rather than copy, so invalid input bytes appear as the
     * U+FFFD they decoded to and the two forms always agree. */
    s->text = av_units_to_text(s->units, s->len, &s->text_len);
}

av_string *av_string_new(const char *value)
{
    return av_string_new_len(value, strlen(value));
}

av_string *av_string_new_len(const char *value, size_t nbytes)
{
    av_string *s = AUSSOM_ALLOC(sizeof(*s));
    av_object_init(&s->base, C_TYPE_STRING);
    set_text(s, value, nbytes);
    return s;
}

av_string *av_string_new_units(const uint16_t *units, size_t n)
{
    av_string *s = AUSSOM_ALLOC(sizeof(*s));
    av_object_init(&s->base, C_TYPE_STRING);
    av_string_set_units(s, units, n);
    return s;
}

const char *av_string_get(const av_string *s) { return s->text; }

const uint16_t *av_string_units(const av_string *s, size_t *n)
{
    if (n != NULL) *n = s->len;
    return s->units;
}

size_t av_string_length(const av_string *s) { return s->len; }

void av_string_set(av_string *s, const char *value)
{
    set_text(s, value, strlen(value));
}

void av_string_set_units(av_string *s, const uint16_t *units, size_t n)
{
    uint16_t *u = AUSSOM_ALLOC_ATOMIC((n + 1) * sizeof(uint16_t));
    if (n > 0) memcpy(u, units, n * sizeof(uint16_t));
    s->units = u;
    s->len = n;
    s->text = av_units_to_text(u, n, &s->text_len);
}

void av_string_write(const av_type *v, int level, FILE *out)
{
    const av_string *s = (const av_string *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cString\",\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"value\": \"%s\"\n", s->text);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_string_write_str(const av_type *v, int level, FILE *out)
{
    const av_string *s = (const av_string *)v;
    if (level > 0) {
        /* str(level>0) quotes the value, matching AussomString.str(int). */
        fprintf(out, "\"%s\"", s->text);
    } else {
        fputs(s->text, out);
    }
}

av_type *av_string_clone(const av_type *v)
{
    const av_string *s = (const av_string *)v;
    return &av_string_new_units(s->units, s->len)->base.base;
}
