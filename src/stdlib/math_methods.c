/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `math` class (com.aussom.stdlib.AMath), backed by libm.
 *
 * The method resolver widens an int argument to satisfy a `double`
 * parameter for overload matching, but it does not convert the stored
 * value, so a passed-in int arrives here still tagged C_TYPE_INT.
 * Every double argument is therefore read through av_numeric_double
 * rather than cast straight to av_double.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "aussom/av.h"
#include "aussom/c_type.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

/* <math.h> only exposes these under _XOPEN_SOURCE / _GNU_SOURCE; the
 * build uses strict _POSIX_C_SOURCE, so define them when absent. */
#ifndef M_E
#define M_E  2.7182818284590452354
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double argd(av_list *a, size_t i) { return av_numeric_double(av_list_get(a, i)); }

static av_type *dret(double v) { return &av_double_new(v)->base.base; }

static bool is_num(av_type *v)
{
    return v->type == C_TYPE_INT || v->type == C_TYPE_DOUBLE || v->type == C_TYPE_BOOL;
}

/* --- constants --------------------------------------------------- */

static av_type *m_e(av_type *self, environment *env, av_list *args)
{ (void)env; (void)self; (void)args; return dret(M_E); }

static av_type *m_pi(av_type *self, environment *env, av_list *args)
{ (void)env; (void)self; (void)args; return dret(M_PI); }

/* --- single-argument libm wrappers ------------------------------- */

static av_type *m_acos(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(acos(argd(a,0))); }
static av_type *m_asin(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(asin(argd(a,0))); }
static av_type *m_atan(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(atan(argd(a,0))); }
static av_type *m_cbrt(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(cbrt(argd(a,0))); }
static av_type *m_ceil(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(ceil(argd(a,0))); }
static av_type *m_cos(av_type *s, environment *e, av_list *a)   { (void)e;(void)s; return dret(cos(argd(a,0))); }
static av_type *m_cosh(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(cosh(argd(a,0))); }
static av_type *m_exp(av_type *s, environment *e, av_list *a)   { (void)e;(void)s; return dret(exp(argd(a,0))); }
static av_type *m_expm1(av_type *s, environment *e, av_list *a) { (void)e;(void)s; return dret(expm1(argd(a,0))); }
static av_type *m_floor(av_type *s, environment *e, av_list *a) { (void)e;(void)s; return dret(floor(argd(a,0))); }
static av_type *m_log(av_type *s, environment *e, av_list *a)   { (void)e;(void)s; return dret(log(argd(a,0))); }
static av_type *m_log10(av_type *s, environment *e, av_list *a) { (void)e;(void)s; return dret(log10(argd(a,0))); }
static av_type *m_log1p(av_type *s, environment *e, av_list *a) { (void)e;(void)s; return dret(log1p(argd(a,0))); }
static av_type *m_nextUp(av_type *s, environment *e, av_list *a){ (void)e;(void)s; return dret(nextafter(argd(a,0), INFINITY)); }
static av_type *m_rint(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(rint(argd(a,0))); }
static av_type *m_sin(av_type *s, environment *e, av_list *a)   { (void)e;(void)s; return dret(sin(argd(a,0))); }
static av_type *m_sinh(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(sinh(argd(a,0))); }
static av_type *m_sqrt(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(sqrt(argd(a,0))); }
static av_type *m_tan(av_type *s, environment *e, av_list *a)   { (void)e;(void)s; return dret(tan(argd(a,0))); }
static av_type *m_tanh(av_type *s, environment *e, av_list *a)  { (void)e;(void)s; return dret(tanh(argd(a,0))); }
static av_type *m_toDeg(av_type *s, environment *e, av_list *a) { (void)e;(void)s; return dret(argd(a,0) * (180.0 / M_PI)); }
static av_type *m_toRad(av_type *s, environment *e, av_list *a) { (void)e;(void)s; return dret(argd(a,0) * (M_PI / 180.0)); }
static av_type *m_ulp(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    double x = argd(a, 0);
    if (isnan(x)) return dret(x);
    if (isinf(x)) return dret(INFINITY);
    /* Math.ulp: the gap above |x| at its exponent, which is still
     * finite for the largest double (2^971). */
    x = fabs(x);
    if (x < DBL_MIN) return dret(0x1p-1074);
    int ex;
    frexp(x, &ex);
    return dret(ldexp(1.0, ex - 53));
}

/* --- two-argument libm wrappers ---------------------------------- */

static av_type *m_copySign(av_type *s, environment *e, av_list *a)
{ (void)e;(void)s; return dret(copysign(argd(a,0), argd(a,1))); }
static av_type *m_hypot(av_type *s, environment *e, av_list *a)
{ (void)e;(void)s; return dret(hypot(argd(a,0), argd(a,1))); }
static av_type *m_IEEEremainder(av_type *s, environment *e, av_list *a)
{ (void)e;(void)s; return dret(remainder(argd(a,0), argd(a,1))); }
static av_type *m_nextAfter(av_type *s, environment *e, av_list *a)
{ (void)e;(void)s; return dret(nextafter(argd(a,0), argd(a,1))); }
static av_type *m_pow(av_type *s, environment *e, av_list *a)
{ (void)e;(void)s; return dret(pow(argd(a,0), argd(a,1))); }

/* --- methods with type-dependent behavior ------------------------ */

static av_type *m_abs(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    av_type *arg = av_list_get(a, 0);
    if (arg->type == C_TYPE_INT) {
        /* Math.abs(Long.MIN_VALUE) is Long.MIN_VALUE; llabs of it is
         * undefined, so negate in unsigned, where it wraps. */
        int64_t v = av_int_get((av_int *)arg);
        return &av_int_new(v < 0 ? (int64_t)(0 - (uint64_t)v) : v)->base.base;
    }
    if (arg->type == C_TYPE_DOUBLE) {
        return dret(fabs(av_double_get((av_double *)arg)));
    }
    if (arg->type == C_TYPE_BOOL) {
        return &av_int_new(0)->base.base;   /* matches Java AMath.abs */
    }
    char m[128];
    snprintf(m, sizeof(m), "math.abs(): Expecting object of type bool, int or "
             "double but found %s instead.", c_type_name(arg->type));
    return &av_exception_new(m)->base.base;
}

/* Math.max and Math.min: NaN if either is NaN (fmax and fmin drop
 * it), and 0.0 above -0.0. */
static double java_max(double x, double y)
{
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0 && y == 0.0) return signbit(x) ? y : x;
    return x > y ? x : y;
}

static double java_min(double x, double y)
{
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0 && y == 0.0) return signbit(x) ? x : y;
    return x < y ? x : y;
}

static av_type *m_max(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    av_type *x = av_list_get(a, 0), *y = av_list_get(a, 1);
    if (!is_num(x) || !is_num(y)) {
        /* Name the argument that is not numeric. */
        av_type *bad = is_num(x) ? y : x;
        char m[128];
        snprintf(m, sizeof(m), "math.max(): Expecting object of type bool, int or "
                 "double but found %s instead.", c_type_name(bad->type));
        return &av_exception_new(m)->base.base;
    }
    return dret(java_max(av_numeric_double(x), av_numeric_double(y)));
}

static av_type *m_min(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    av_type *x = av_list_get(a, 0), *y = av_list_get(a, 1);
    if (!is_num(x) || !is_num(y)) {
        /* Name the argument that is not numeric. */
        av_type *bad = is_num(x) ? y : x;
        char m[128];
        snprintf(m, sizeof(m), "math.min(): Expecting object of type bool, int or "
                 "double but found %s instead.", c_type_name(bad->type));
        return &av_exception_new(m)->base.base;
    }
    return dret(java_min(av_numeric_double(x), av_numeric_double(y)));
}

static av_type *m_getExponent(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    double x = argd(a, 0);
    if (isnan(x) || isinf(x)) return dret(1024.0);   /* Java MAX_EXPONENT + 1 */
    if (x == 0.0) return dret(-1023.0);               /* Java MIN_EXPONENT - 1 */
    return dret((double)ilogb(x));
}

/* Math.round(double) is a long, widened back to double: NaN is 0,
 * values past a long clamp to its ends, and halves round up without
 * the error floor(x + 0.5) makes for 0.49999999999999994. */
static av_type *m_round(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    double x = argd(a, 0);
    if (isnan(x)) return dret(0.0);
    if (x >= 0x1p63) return dret((double)INT64_MAX);
    if (x <= -0x1p63) return dret((double)INT64_MIN);
    double f = floor(x);
    int64_t r = (int64_t)f + ((x - f) >= 0.5 ? 1 : 0);
    return dret((double)r);
}

static av_type *m_scalb(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    int64_t scale = av_numeric_int(av_list_get(a, 1));
    av_type *err = stdlib_check_int_range("math.scalb", scale);
    if (err != NULL) return err;
    double x = argd(a, 0);
    return dret(scalbn(x, (int)scale));
}

static av_type *m_signum(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s;
    double x = argd(a, 0);
    if (isnan(x)) return dret(x);
    if (x > 0.0) return dret(1.0);
    if (x < 0.0) return dret(-1.0);
    return dret(x);   /* preserves +0.0 / -0.0 */
}

static av_type *m_rand(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s; (void)a;
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = true; }
    return dret((double)rand() / ((double)RAND_MAX + 1.0));
}

const aussom_extern_method stdlib_amath_methods[] = {
    { "e", m_e },
    { "pi", m_pi },
    { "abs", m_abs },
    { "acos", m_acos },
    { "asin", m_asin },
    { "atan", m_atan },
    { "cbrt", m_cbrt },
    { "ceil", m_ceil },
    { "copySign", m_copySign },
    { "cos", m_cos },
    { "cosh", m_cosh },
    { "exp", m_exp },
    { "expm1", m_expm1 },
    { "floor", m_floor },
    { "getExponent", m_getExponent },
    { "hypot", m_hypot },
    { "IEEEremainder", m_IEEEremainder },
    { "log", m_log },
    { "log10", m_log10 },
    { "log1p", m_log1p },
    { "max", m_max },
    { "min", m_min },
    { "nextAfter", m_nextAfter },
    { "nextUp", m_nextUp },
    { "pow", m_pow },
    { "rand", m_rand },
    { "rint", m_rint },
    { "round", m_round },
    { "scalb", m_scalb },
    { "signum", m_signum },
    { "sin", m_sin },
    { "sinh", m_sinh },
    { "sqrt", m_sqrt },
    { "tan", m_tan },
    { "tanh", m_tanh },
    { "toDeg", m_toDeg },
    { "toRad", m_toRad },
    { "ulp", m_ulp },
    { NULL, NULL }
};
