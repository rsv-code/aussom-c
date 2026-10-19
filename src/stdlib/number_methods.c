/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The numeric / bool primitive types (int, double, bool). Mirrors
 * com.aussom.types.AussomInt / AussomDouble / AussomBool.
 */

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"
#include "unicode.h"

/* --- shared conversions ------------------------------------------ */

static av_type *prim_toString(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_string_new(av_str(self))->base.base;
}

static av_type *prim_toInt(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_int_new(av_numeric_int(self))->base.base;
}

static av_type *prim_toDouble(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_double_new(av_numeric_double(self))->base.base;
}

static av_type *prim_toBool(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_bool_new(av_numeric_bool(self))->base.base;
}

static av_type *prim_toJson(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return av_to_json(self);
}

/* --- int --------------------------------------------------------- */

static int64_t self_i(av_type *self) { return av_int_get((av_int *)self); }
static int64_t arg_i(av_list *a, size_t i) { int64_t v = 0; av_list_get_int(a, i, &v); return v; }

static av_type *int_compare(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t a = self_i(self), b = arg_i(args, 0);
    return &av_int_new((a > b) - (a < b))->base.base;
}

static av_type *int_signum(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    int64_t v = self_i(self);
    return &av_int_new((v > 0) - (v < 0))->base.base;
}

static av_type *int_numLeadingZeros(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    uint64_t v = (uint64_t)self_i(self);
    return &av_int_new(v == 0 ? 64 : __builtin_clzll(v))->base.base;
}

static av_type *int_numTrailingZeros(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    uint64_t v = (uint64_t)self_i(self);
    return &av_int_new(v == 0 ? 64 : __builtin_ctzll(v))->base.base;
}

static av_type *int_reverse(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    uint64_t v = (uint64_t)self_i(self), r = 0;
    for (int i = 0; i < 64; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return &av_int_new((int64_t)r)->base.base;
}

static av_type *int_reverseBytes(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_int_new((int64_t)__builtin_bswap64((uint64_t)self_i(self)))->base.base;
}

static av_type *int_rotateLeft(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t distance = arg_i(args, 0);
    av_type *err = stdlib_check_int_range("int.rotateLeft", distance);
    if (err != NULL) return err;
    uint64_t v = (uint64_t)self_i(self);
    unsigned d = (unsigned)(distance & 63);
    uint64_t r = (d == 0) ? v : (v << d) | (v >> (64 - d));
    return &av_int_new((int64_t)r)->base.base;
}

static av_type *int_rotateRight(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t distance = arg_i(args, 0);
    av_type *err = stdlib_check_int_range("int.rotateRight", distance);
    if (err != NULL) return err;
    uint64_t v = (uint64_t)self_i(self);
    unsigned d = (unsigned)(distance & 63);
    uint64_t r = (d == 0) ? v : (v >> d) | (v << (64 - d));
    return &av_int_new((int64_t)r)->base.base;
}

static av_type *int_toBinary(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    uint64_t v = (uint64_t)self_i(self);
    char buf[65];
    buf[64] = '\0';
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return &av_string_new(buf)->base.base; }
    int i = 64;
    while (v) { buf[--i] = (char)('0' + (v & 1)); v >>= 1; }
    return &av_string_new(buf + i)->base.base;
}

static av_type *int_toHex(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIx64, (uint64_t)self_i(self));
    return &av_string_new(buf)->base.base;
}

static av_type *int_toOctal(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIo64, (uint64_t)self_i(self));
    return &av_string_new(buf)->base.base;
}

/* Character.digit(c, 36) for one UTF-16 unit: a Unicode decimal digit
 * is its value, and an ASCII or fullwidth Latin letter is 10 to 35. */
static int java_digit(uint16_t c)
{
    int d = uni_digit(c);
    if (d >= 0) return d;
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 0xFF41 + 10;
    if (c >= 0xFF21 && c <= 0xFF3A) return c - 0xFF21 + 10;
    return -1;
}

/* Long.parseLong(s, radix): an optional sign, then at least one digit
 * of the radix, nothing else (no spaces, no 0x). A radix outside 2..36
 * or a value outside a long fails. Digits are read per UTF-16 unit with
 * Character.digit, so any Unicode decimal digit counts. */
static bool java_parse_long(const char *text, int radix, int64_t *out)
{
    size_t n;
    uint16_t *s = av_text_to_units(text, strlen(text), &n);
    if (radix < 2 || radix > 36 || n == 0) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') { neg = (s[0] == '-'); i = 1; }
    if (i == n) return false;
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t v = 0;
    for (; i < n; i++) {
        int d = java_digit(s[i]);
        if (d < 0) return false;
        if (d >= radix) return false;
        if (v > (limit - (uint64_t)d) / (uint64_t)radix) return false;
        v = v * (uint64_t)radix + (uint64_t)d;
    }
    *out = neg ? (int64_t)(0u - v) : (int64_t)v;
    return true;
}

/* AussomInt.parse and SInt.parse. Any failure, a null argument
 * included, is the same returned exception; only the class name in it
 * differs. The value method (AussomInt.parse) sets its receiver and
 * returns it; the static Int.parse (SInt) returns a new int. */
static av_type *parse_long(av_type *self, av_list *args, const char *method,
                           const char *msg)
{
    const char *s = av_list_get_string(args, 0);
    int radix = 10;
    av_type *r = av_list_size(args) > 1 ? av_list_get(args, 1) : NULL;
    if (r != NULL && r->type == C_TYPE_INT) {
        int64_t rv = arg_i(args, 1);
        av_type *err = stdlib_check_int_range(method, rv);
        if (err != NULL) return err;
        radix = (rv < 2 || rv > 36) ? 0 : (int)rv;
    }
    int64_t v;
    if (s == NULL || !java_parse_long(s, radix, &v))
        return &av_exception_new(msg)->base.base;
    if (self != NULL && self->type == C_TYPE_INT) {
        av_int_set((av_int *)self, v);
        return self;
    }
    return &av_int_new(v)->base.base;
}

static av_type *int_parse(av_type *self, environment *env, av_list *args)
{
    (void)env;
    return parse_long(self, args, "int.parse", "int.parse(): Integer parse exception.");
}

static av_type *sint_parse(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self;
    return parse_long(NULL, args, "Int.parse", "Int.parse(): Integer parse exception.");
}

/* --- double ------------------------------------------------------ */

static av_type *dbl_compare(av_type *self, environment *env, av_list *args)
{
    (void)env;
    double a = av_double_get((av_double *)self);
    double b;
    if (!av_list_get_double(args, 0, &b)) return &av_exception_new("dbl_compare: argument 1 must be a double.")->base.base;
    /* Double.compare: -0.0 is below 0.0, and NaN is above everything,
     * itself included equal. */
    int r;
    if (a < b) r = -1;
    else if (a > b) r = 1;
    else {
        bool an = isnan(a), bn = isnan(b);
        if (an || bn) r = an == bn ? 0 : (an ? 1 : -1);
        else {
            bool as = signbit(a) != 0, bs = signbit(b) != 0;
            r = as == bs ? 0 : (as ? -1 : 1);
        }
    }
    return &av_int_new(r)->base.base;
}

static av_type *dbl_isInfinite(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_bool_new(isinf(av_double_get((av_double *)self)) != 0)->base.base;
}

static av_type *dbl_isNan(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_bool_new(isnan(av_double_get((av_double *)self)) != 0)->base.base;
}

/* Double.toHexString: 0x1.<hex>p<exp> for a normal value and
 * 0x0.<hex>p-1022 for a subnormal, with the fraction's trailing zeros
 * dropped down to one digit. */
static av_type *dbl_toHex(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    double d = av_double_get((av_double *)self);
    if (isnan(d)) return &av_string_new("NaN")->base.base;
    if (isinf(d)) return &av_string_new(d < 0 ? "-Infinity" : "Infinity")->base.base;
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    bool neg = (bits >> 63) != 0;
    int bexp = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    char hex[16];
    snprintf(hex, sizeof(hex), "%013llx", (unsigned long long)frac);
    size_t hl = strlen(hex);
    while (hl > 1 && hex[hl - 1] == '0') hl--;
    hex[hl] = '\0';
    char buf[64];
    if (bexp == 0 && frac == 0)
        snprintf(buf, sizeof(buf), "%s0x0.0p0", neg ? "-" : "");
    else if (bexp == 0)
        snprintf(buf, sizeof(buf), "%s0x0.%sp-1022", neg ? "-" : "", hex);
    else
        snprintf(buf, sizeof(buf), "%s0x1.%sp%d", neg ? "-" : "", hex, bexp - 1023);
    return &av_string_new(buf)->base.base;
}

/* Double.parseDouble: surrounding characters at or below ' ' are
 * trimmed; then an optional sign and NaN, Infinity, a hex float with a
 * p exponent, or a decimal with an optional exponent, followed by an
 * optional f/F/d/D. */
static bool java_parse_double(const char *text, double *out)
{
    /* Trimmed by UTF-16 unit, so U+0000 counts; what remains must be
     * ASCII. */
    size_t n;
    uint16_t *u = av_text_to_units(text, strlen(text), &n);
    size_t a = 0;
    while (a < n && u[a] <= ' ') a++;
    while (n > a && u[n - 1] <= ' ') n--;
    if (a == n) return false;
    char *t = AUSSOM_ALLOC_ATOMIC(n - a + 1);
    for (size_t k = a; k < n; k++) {
        if (u[k] > 0x7F) return false;
        t[k - a] = (char)u[k];
    }
    t[n - a] = '\0';
    size_t len = n - a;
    size_t i = 0;
    bool neg = false;
    if (t[0] == '+' || t[0] == '-') { neg = (t[0] == '-'); i = 1; }
    if (strcmp(t + i, "NaN") == 0) { *out = NAN; return true; }
    if (strcmp(t + i, "Infinity") == 0) { *out = neg ? -INFINITY : INFINITY; return true; }
    /* A trailing type letter is allowed and ignored. */
    if (len > i && strchr("fFdD", t[len - 1]) != NULL
        && !(len > i + 1 && (t[i + 1] == 'x' || t[i + 1] == 'X') && strchr(t, 'p') == NULL && strchr(t, 'P') == NULL))
        t[--len] = '\0';
    size_t j = i;
    if (t[j] == '0' && (t[j + 1] == 'x' || t[j + 1] == 'X')) {
        j += 2;
        size_t digs = 0;
        while (isxdigit((unsigned char)t[j])) { j++; digs++; }
        if (t[j] == '.') { j++; while (isxdigit((unsigned char)t[j])) { j++; digs++; } }
        if (digs == 0 || (t[j] != 'p' && t[j] != 'P')) return false;
        j++;
        if (t[j] == '+' || t[j] == '-') j++;
        if (!isdigit((unsigned char)t[j])) return false;
        while (isdigit((unsigned char)t[j])) j++;
    } else {
        size_t digs = 0;
        while (isdigit((unsigned char)t[j])) { j++; digs++; }
        if (t[j] == '.') { j++; while (isdigit((unsigned char)t[j])) { j++; digs++; } }
        if (digs == 0) return false;
        if (t[j] == 'e' || t[j] == 'E') {
            j++;
            if (t[j] == '+' || t[j] == '-') j++;
            if (!isdigit((unsigned char)t[j])) return false;
            while (isdigit((unsigned char)t[j])) j++;
        }
    }
    if (t[j] != '\0') return false;
    *out = strtod(t, NULL);
    return true;
}

/* AussomDouble.parse and SDouble.parse: any failure, a null argument
 * included, is the same returned exception. The value method
 * (AussomDouble.parse) sets its receiver and returns it; the static
 * Double.parse (SDouble) returns a new double. */
static av_type *parse_double(av_type *self, av_list *args, const char *msg)
{
    const char *s = av_list_get_string(args, 0);
    double v;
    if (s == NULL || !java_parse_double(s, &v))
        return &av_exception_new(msg)->base.base;
    if (self != NULL && self->type == C_TYPE_DOUBLE) {
        av_double_set((av_double *)self, v);
        return self;
    }
    return &av_double_new(v)->base.base;
}

static av_type *dbl_parse(av_type *self, environment *env, av_list *args)
{
    (void)env;
    return parse_double(self, args, "double.parse(): Double parse exception.");
}

static av_type *sdbl_parse(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self;
    return parse_double(NULL, args, "Double.parse(): Double parse exception.");
}

/* --- bool -------------------------------------------------------- */

static av_type *bool_compare(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int a = av_bool_get((av_bool *)self) ? 1 : 0;
    bool bb;
    if (!av_list_get_bool(args, 0, &bb)) return &av_exception_new("bool.compare(): argument 1 must be a bool.")->base.base;
    int b = bb ? 1 : 0;
    return &av_int_new((a > b) - (a < b))->base.base;
}

static av_type *bool_parse(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self;
    const char *s = av_list_get_string(args, 0);
    if (s == NULL) return &av_exception_new("bool_parse: argument 1 must be a string.")->base.base;
    /* Boolean.parseBoolean: true iff "true" ignoring case. The value
     * method (AussomBool.parse) sets its receiver and returns it; the
     * static Bool.parse (SBool) returns a new bool. */
    bool v = strcasecmp(s, "true") == 0;
    if (self != NULL && self->type == C_TYPE_BOOL) {
        av_bool_set((av_bool *)self, v);
        return self;
    }
    return &av_bool_new(v)->base.base;
}

/* --- static Int / Double / Bool classes (SInt / SDouble / SBool) -- */

static av_type *sint_maxVal(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_int_new(INT64_MAX)->base.base; }
static av_type *sint_minVal(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_int_new(INT64_MIN)->base.base; }

static av_type *sdbl_maxExp(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_int_new(1023)->base.base; }
static av_type *sdbl_minExp(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_int_new(-1022)->base.base; }
static av_type *sdbl_size(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_int_new(64)->base.base; }
static av_type *sdbl_maxVal(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_double_new(DBL_MAX)->base.base; }
static av_type *sdbl_minNormal(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_double_new(DBL_MIN)->base.base; }
static av_type *sdbl_minVal(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_double_new(0x1p-1074)->base.base; }  /* smallest subnormal */
static av_type *sdbl_nanVal(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_double_new(NAN)->base.base; }
static av_type *sdbl_posInfinity(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_double_new(INFINITY)->base.base; }
static av_type *sdbl_negInfinity(av_type *s, environment *e, av_list *a)
{ (void)s; (void)e; (void)a; return &av_double_new(-INFINITY)->base.base; }

const aussom_extern_method stdlib_aussom_int_methods[] = {
    { "toString", prim_toString },
    { "toDouble", prim_toDouble },
    { "toBool", prim_toBool },
    { "compare", int_compare },
    { "signum", int_signum },
    { "numLeadingZeros", int_numLeadingZeros },
    { "numTrailingZeros", int_numTrailingZeros },
    { "reverse", int_reverse },
    { "reverseBytes", int_reverseBytes },
    { "rotateLeft", int_rotateLeft },
    { "rotateRight", int_rotateRight },
    { "toBinary", int_toBinary },
    { "toHex", int_toHex },
    { "toOctal", int_toOctal },
    { "parse", int_parse },
    { "toJson", prim_toJson },
    { "pack", stdlib_pack },
    { NULL, NULL }
};

const aussom_extern_method stdlib_aussom_double_methods[] = {
    { "toString", prim_toString },
    { "toInt", prim_toInt },
    { "toBool", prim_toBool },
    { "compare", dbl_compare },
    { "isInfinite", dbl_isInfinite },
    { "isNan", dbl_isNan },
    { "toHex", dbl_toHex },
    { "parse", dbl_parse },
    { "toJson", prim_toJson },
    { "pack", stdlib_pack },
    { NULL, NULL }
};

const aussom_extern_method stdlib_aussom_bool_methods[] = {
    { "toString", prim_toString },
    { "toInt", prim_toInt },
    { "toDouble", prim_toDouble },
    { "compare", bool_compare },
    { "parse", bool_parse },
    { "toJson", prim_toJson },
    { "pack", stdlib_pack },
    { NULL, NULL }
};

/* Static classes Int / Double / Bool (SInt / SDouble / SBool). */
const aussom_extern_method stdlib_sint_methods[] = {
    { "maxVal", sint_maxVal },
    { "minVal", sint_minVal },
    { "parse", sint_parse },
    { NULL, NULL }
};

const aussom_extern_method stdlib_sdouble_methods[] = {
    { "maxExp", sdbl_maxExp },
    { "minExp", sdbl_minExp },
    { "size", sdbl_size },
    { "maxVal", sdbl_maxVal },
    { "minNormal", sdbl_minNormal },
    { "minVal", sdbl_minVal },
    { "nanVal", sdbl_nanVal },
    { "posInfinity", sdbl_posInfinity },
    { "negInfinity", sdbl_negInfinity },
    { "parse", sdbl_parse },
    { NULL, NULL }
};

const aussom_extern_method stdlib_sbool_methods[] = {
    { "parse", bool_parse },
    { NULL, NULL }
};
