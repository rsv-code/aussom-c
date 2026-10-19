/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_double.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_double *av_double_new(double value)
{
    av_double *d = AUSSOM_ALLOC(sizeof(*d));
    av_object_init(&d->base, C_TYPE_DOUBLE);
    d->value = value;
    return d;
}

double av_double_get(const av_double *d) { return d->value; }
void av_double_set(av_double *d, double value) { d->value = value; }

/* Java's Double.toString (JDK 19 and later): the shortest decimal
 * that reads back as the same double, the closest one when several
 * qualify. Values from 1e-3 up to 1e7 print as a plain decimal with at
 * least one digit after the point (100.0, 0.001); the rest print as
 * d.dddE<n> (1.0E10, 1.0E-4). */
void av_double_java_str(double d, char *buf, size_t n)
{
    if (isnan(d)) { snprintf(buf, n, "NaN"); return; }
    if (isinf(d)) { snprintf(buf, n, d < 0 ? "-Infinity" : "Infinity"); return; }
    if (d == 0.0) { snprintf(buf, n, signbit(d) ? "-0.0" : "0.0"); return; }

    /* Shortest round-trip digits: %.Ne is the closest (N+1)-digit
     * decimal, so the first N that reads back is the answer. Java
     * never goes below two digits: when one would do, it takes the
     * closest two-digit decimal (4.9E-324, not 5.0E-324). */
    char sci[40];
    for (int p = 1; p < 17; p++) {
        snprintf(sci, sizeof(sci), "%.*e", p, d);
        if (strtod(sci, NULL) == d) break;
    }
    /* sci is [-]d[.ddd]e[+-]xx: split into digits and exponent. */
    const char *q = sci;
    bool neg = false;
    if (*q == '-') { neg = true; q++; }
    char digits[24];
    size_t nd = 0;
    for (; *q != 'e'; q++) if (*q != '.') digits[nd++] = *q;
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = '\0';
    int exp = atoi(q + 1);

    char out[64];
    size_t o = 0;
    if (neg) out[o++] = '-';
    double ad = fabs(d);
    if (ad >= 1e-3 && ad < 1e7) {
        if (exp >= 0) {
            for (int i = 0; i <= exp; i++) out[o++] = (i < (int)nd) ? digits[i] : '0';
            out[o++] = '.';
            if ((int)nd > exp + 1) {
                for (int i = exp + 1; i < (int)nd; i++) out[o++] = digits[i];
            } else {
                out[o++] = '0';
            }
        } else {
            out[o++] = '0';
            out[o++] = '.';
            for (int i = 0; i < -exp - 1; i++) out[o++] = '0';
            for (size_t i = 0; i < nd; i++) out[o++] = digits[i];
        }
        out[o] = '\0';
    } else {
        out[o++] = digits[0];
        out[o++] = '.';
        if (nd > 1) for (size_t i = 1; i < nd; i++) out[o++] = digits[i];
        else out[o++] = '0';
        snprintf(out + o, sizeof(out) - o, "E%d", exp);
    }
    snprintf(buf, n, "%s", out);
}

void av_double_write(const av_type *v, int level, FILE *out)
{
    const av_double *d = (const av_double *)v;
    char num[64];
    av_double_java_str(d->value, num, sizeof(num));
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cDouble\",\n", out);
    av_write_tabs(level + 1, out);
    fprintf(out, "\"value\": %s\n", num);
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_double_write_str(const av_type *v, int level, FILE *out)
{
    (void)level;
    char num[64];
    av_double_java_str(((const av_double *)v)->value, num, sizeof(num));
    fputs(num, out);
}

av_type *av_double_clone(const av_type *v)
{
    const av_double *d = (const av_double *)v;
    return &av_double_new(d->value)->base.base;
}
