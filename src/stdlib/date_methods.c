/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The Date extern type (com.aussom.stdlib.ADate).
 *
 * Building blocks:
 *
 * - A Date is a java.time.Instant: whole seconds since 1970-01-01T00:00Z
 *   plus a nanosecond part (0 - 999,999,999). Its range is Instant's,
 *   years -1,000,000,000 to 1,000,000,000.
 * - Every field accessor reads the Instant at UTC, as ADate does with
 *   atZone(ZoneOffset.UTC). Like Java's LocalDateTime, that view covers
 *   years -999,999,999 to 999,999,999; outside it an accessor throws.
 * - The calendar math is done here (proleptic Gregorian), not through
 *   the C library, so it is the same on every platform. 32-bit Android
 *   has a 32-bit time_t that cannot hold dates past 2038.
 * - Overflow follows the Java methods step by step and reports Java's
 *   message: "long overflow" (ArithmeticException), "Instant exceeds
 *   minimum or maximum instant", and the field range messages.
 *
 * format() and parse() take Java DateTimeFormatter patterns; the
 * patterns are compiled, printed and parsed in date_pattern.c.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aussom/memstream.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/av_object.h"
#include "aussom/ast_eval.h"
#include "aussom/engine.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"
#include "date_pattern.h"

/* First-field tag so an accessor can tell a real date_state from a
 * plain av_object (a confused `extern class ... : com.aussom.stdlib.ADate`
 * binding, or a non-Date passed to a comparison). Reading it is always
 * in bounds. */
#define DATE_MAGIC 0x44415445u   /* "DATE" */

#define NANOS_PER_SECOND 1000000000LL
#define SECONDS_PER_DAY  86400LL
#define NANOS_PER_DAY    (SECONDS_PER_DAY * NANOS_PER_SECOND)

/* Instant.MIN / Instant.MAX in epoch seconds. */
#define INSTANT_MIN_SECOND (-31557014167219200LL)
#define INSTANT_MAX_SECOND (31556889864403199LL)

/* LocalDate's epoch-day range (years -999,999,999 to 999,999,999). */
#define EPOCH_DAY_MIN (-365243219162LL)
#define EPOCH_DAY_MAX (365241780471LL)

#define YEAR_MIN (-999999999LL)
#define YEAR_MAX (999999999LL)

/* An Instant: epoch seconds plus 0 - 999,999,999 nanoseconds. */
typedef struct date_state { uint32_t magic; int64_t secs; int32_t nanos; } date_state;

/* The receiver's real date_state. A receiver that was never
 * initialized as a Date (a script's own ADate binding, or a subclass
 * carrying a copied parent object) gets a fresh epoch date attached, as
 * Java's ADate is built when the object is. Returning a valid pointer
 * either way keeps every one-line accessor memory-safe without its own
 * check. */
static date_state *self_date(av_type *self)
{
    if (self != NULL && self->type == C_TYPE_OBJECT) {
        void *eo = av_object_get_extern_object((av_object *)self);
        if (eo != NULL && eo != (void *)self &&
            ((date_state *)eo)->magic == DATE_MAGIC) {
            return (date_state *)eo;
        }
    }
    date_state *d = AUSSOM_ALLOC(sizeof(date_state));
    d->magic = DATE_MAGIC;
    d->secs = 0;
    d->nanos = 0;
    if (self != NULL && self->type == C_TYPE_OBJECT)
        av_object_set_extern_object((av_object *)self, d);
    return d;
}

static av_type *date_exc(const char *msg) { return &av_exception_new(msg)->base.base; }

/* A DateTimeException thrown by java.time. ADate does not catch it, so
 * the script sees it as thrown from the method (EXTERN_INVOCATION_
 * TARGET_EXCEPTION, named by the caller). `fn` is kept for the call
 * sites; the method's name comes from the call. */
static av_type *date_fn_exc(const char *fn, const char *what)
{
    (void)fn;
    char msg[256];
    snprintf(msg, sizeof(msg), "java.time.DateTimeException: %s", what);
    return &av_exception_new_thrown(msg)->base.base;
}

/* Math.multiplyExact / addExact failing: a thrown ArithmeticException. */
static av_type *long_overflow(const char *fn)
{
    (void)fn;
    return &av_exception_new_thrown("java.lang.ArithmeticException: long overflow")->base.base;
}

/* DateTimeException from a field's checkValidValue. */
static av_type *field_value_exc(const char *fn, const char *field,
                                const char *range, long long v)
{
    char msg[192];
    snprintf(msg, sizeof(msg), "Invalid value for %s (valid values %s): %lld", field, range, v);
    return date_fn_exc(fn, msg);
}

/* A setter's argument, checked to fit an int (Util.checkIntRange)
 * before it is narrowed. Returns the exception, or NULL and sets *out. */
static av_type *date_int_arg(av_list *args, const char *fn, int *out)
{
    int64_t value = av_numeric_int(av_list_get(args, 0));
    char method[32];
    snprintf(method, sizeof(method), "Date.%s", fn);
    av_type *err = stdlib_check_int_range(method, value);
    if (err != NULL) return err;
    *out = (int)value;
    return NULL;
}

/* What Java's ZonedDateTime.withX does with a value outside the field's
 * range: it throws DateTimeException ("Invalid value for HourOfDay
 * (valid values 0 - 23): 25"). Here that becomes the exception, with
 * the method named. */
static bool field_in_range(int v, int64_t lo, int64_t hi)
{
    return v >= lo && v <= hi;
}

static av_type *field_range_exc(const char *fn, const char *field,
                                const char *range, int v)
{
    return field_value_exc(fn, field, range, v);
}

/* Java long addition, which wraps instead of overflowing. */
static int64_t wrap_add(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

/* --- Instant ------------------------------------------------------ */

/* Instant.ofEpochSecond(secs, nano_adj): stores the result in `d`, or
 * returns Java's exception for method `fn` and leaves `d` unchanged. */
static av_type *instant_set(date_state *d, int64_t secs, int64_t nano_adj, const char *fn)
{
    int64_t s;
    if (__builtin_add_overflow(secs, date_floordiv(nano_adj, NANOS_PER_SECOND), &s)) {
        return long_overflow(fn);
    }
    if (s < INSTANT_MIN_SECOND || s > INSTANT_MAX_SECOND) {
        return date_fn_exc(fn, "Instant exceeds minimum or maximum instant");
    }
    d->secs = s;
    d->nanos = (int32_t)date_floormod(nano_adj, NANOS_PER_SECOND);
    return NULL;
}

/* Instant.plus(secondsToAdd, nanosToAdd). */
static av_type *instant_plus(date_state *d, int64_t secs_add, int64_t nanos_add, const char *fn)
{
    if (secs_add == 0 && nanos_add == 0) return NULL;
    int64_t es;
    if (__builtin_add_overflow(d->secs, secs_add, &es)) return long_overflow(fn);
    if (__builtin_add_overflow(es, nanos_add / NANOS_PER_SECOND, &es)) return long_overflow(fn);
    nanos_add %= NANOS_PER_SECOND;
    return instant_set(d, es, d->nanos + nanos_add, fn);
}

/* Instant.ofEpochMilli. Every long is in range. */
static void instant_from_millis(date_state *d, int64_t millis)
{
    d->secs = date_floordiv(millis, 1000);
    d->nanos = (int32_t)(date_floormod(millis, 1000) * 1000000);
}

/* Instant.toEpochMilli. False on long overflow. */
static bool instant_to_millis(const date_state *d, int64_t *out)
{
    int64_t ms;
    if (d->secs < 0 && d->nanos > 0) {
        if (__builtin_mul_overflow(d->secs + 1, (int64_t)1000, &ms)) return false;
        return !__builtin_add_overflow(ms, (int64_t)(d->nanos / 1000000 - 1000), out);
    }
    if (__builtin_mul_overflow(d->secs, (int64_t)1000, &ms)) return false;
    return !__builtin_add_overflow(ms, (int64_t)(d->nanos / 1000000), out);
}

/* Instant.now(), to the nanosecond as on Linux Java. */
static void instant_now(date_state *d)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    d->secs = (int64_t)ts.tv_sec;
    d->nanos = (int32_t)ts.tv_nsec;
}

static int instant_compare(const date_state *a, const date_state *b)
{
    if (a->secs != b->secs) return (a->secs > b->secs) - (a->secs < b->secs);
    return (a->nanos > b->nanos) - (a->nanos < b->nanos);
}

/* --- civil calendar (UTC, proleptic Gregorian) -------------------- */

/* The Instant read at UTC: a LocalDateTime. */
typedef struct {
    int64_t epoch_day;
    int64_t year;
    int mon, day, hour, min, sec, nanos;
} broken;

/* instant.atZone(UTC). NULL on success, else the DateTimeException
 * Java's LocalDate.ofEpochDay throws for a day past year 999,999,999. */
static av_type *decompose(const date_state *d, broken *b, const char *fn)
{
    int64_t day = date_floordiv(d->secs, SECONDS_PER_DAY);
    if (day < EPOCH_DAY_MIN || day > EPOCH_DAY_MAX) {
        return field_value_exc(fn, "EpochDay", "-365243219162 - 365241780471", day);
    }
    int64_t sod = d->secs - day * SECONDS_PER_DAY;
    b->epoch_day = day;
    date_civil_from_days(day, &b->year, &b->mon, &b->day);
    b->hour = (int)(sod / 3600);
    b->min = (int)((sod / 60) % 60);
    b->sec = (int)(sod % 60);
    b->nanos = d->nanos;
    return NULL;
}

/* zdt.toInstant() for fields Java has already validated. Always in
 * Instant's range. */
static void recompose(const broken *b, date_state *d)
{
    d->secs = date_days_from_civil(b->year, b->mon, b->day) * SECONDS_PER_DAY
              + (int64_t)b->hour * 3600 + (int64_t)b->min * 60 + b->sec;
    d->nanos = b->nanos;
}

/* ISO day of week, Monday = 1 .. Sunday = 7. */
static int iso_dow(int64_t epoch_day)
{
    return (int)date_floormod(epoch_day + 3, 7) + 1;
}

static int day_of_year(const broken *b)
{
    return (int)(b->epoch_day - date_days_from_civil(b->year, 1, 1)) + 1;
}

/* Declares `broken b` for the receiver read at UTC, returning Java's
 * exception from the method when that fails. */
#define DECOMPOSE_OR_RETURN(d, fn)                                            \
    broken b;                                                                 \
    do { av_type *e_ = decompose((d), &b, (fn)); if (e_ != NULL) return e_; } while (0)

/* --- ISO text --------------------------------------------------- */

/* Reads `min` to `max` digits. */
static bool read_digits_range(const char **sp, int min, int max, int64_t *out)
{
    const char *s = *sp;
    int64_t v = 0;
    int got = 0;
    while (*s >= '0' && *s <= '9' && got < max) { v = v * 10 + (*s - '0'); s++; got++; }
    if (got < min) return false;
    *out = v;
    *sp = s;
    return true;
}

/* DateTimeFormatter.ISO_OFFSET_DATE_TIME, as json.unpack reads a Date:
 * a year of at least 4 digits ('+' required past 4, '-' for a negative
 * year), -MM-dd'T'HH:mm, optional :ss with an optional fraction of up
 * to 9 digits, then Z or an offset +HH:MM[:ss]. Strict: every field
 * must be a real date and time. */
static bool parse_iso_offset(const char *text, date_state *out)
{
    const char *s = text;
    broken b = { 0, 0, 1, 1, 0, 0, 0, 0 };
    int64_t v;

    int sign = 0;
    if (*s == '+') { sign = 1; s++; }
    else if (*s == '-') { sign = -1; s++; }
    const char *digits = s;
    if (!read_digits_range(&s, 4, 10, &v)) return false;
    size_t got = (size_t)(s - digits);
    if ((sign > 0 && got <= 4) || (sign == 0 && got > 4)) return false;
    b.year = (sign < 0) ? -v : v;
    if (b.year < YEAR_MIN || b.year > YEAR_MAX) return false;

    if (*s++ != '-' || !read_digits_range(&s, 2, 2, &v)) return false;
    b.mon = (int)v;
    if (*s++ != '-' || !read_digits_range(&s, 2, 2, &v)) return false;
    b.day = (int)v;
    if (*s++ != 'T' || !read_digits_range(&s, 2, 2, &v)) return false;
    b.hour = (int)v;
    if (*s++ != ':' || !read_digits_range(&s, 2, 2, &v)) return false;
    b.min = (int)v;
    if (*s == ':') {
        s++;
        if (!read_digits_range(&s, 2, 2, &v)) return false;
        b.sec = (int)v;
        if (*s == '.') {
            s++;
            const char *f0 = s;
            if (!read_digits_range(&s, 0, 9, &v)) return false;
            for (size_t i = (size_t)(s - f0); i < 9; i++) v *= 10;
            b.nanos = (int)v;
        }
    }
    if (b.mon < 1 || b.mon > 12 || b.day < 1 || b.day > date_days_in_month(b.year, b.mon) ||
        b.hour > 23 || b.min > 59 || b.sec > 59) return false;

    int64_t offset = 0;
    if (*s == 'Z') {
        s++;
    } else if (*s == '+' || *s == '-') {
        int osign = (*s == '-') ? -1 : 1;
        s++;
        int64_t oh, om, os = 0;
        if (!read_digits_range(&s, 2, 2, &oh) || *s++ != ':' ||
            !read_digits_range(&s, 2, 2, &om)) return false;
        if (*s == ':') {
            s++;
            if (!read_digits_range(&s, 2, 2, &os)) return false;
        }
        if (oh > 18 || om > 59 || os > 59) return false;
        offset = osign * (oh * 3600 + om * 60 + os);
        if (offset > 18 * 3600 || offset < -18 * 3600) return false;
    } else {
        return false;
    }
    if (*s != '\0') return false;

    recompose(&b, out);
    out->secs -= offset;
    return true;
}

/* --- canonical toString ------------------------------------------ */

/* ADate.toString: yyyy-MM-dd'T'HH:mm:ss.SSSXXX at UTC. NULL with *err
 * set when the date is past what a LocalDateTime can show. */
static char *date_iso(const date_state *d, const char *fn, av_type **err)
{
    broken b;
    av_type *e = decompose(d, &b, fn);
    if (e != NULL) { if (err != NULL) *err = e; return NULL; }
    static const char iso[] = "yyyy-MM-dd'T'HH:mm:ss.SSSXXX";
    uint16_t pat[sizeof(iso) - 1];
    for (size_t i = 0; i < sizeof(pat) / sizeof(pat[0]); i++) pat[i] = (uint8_t)iso[i];
    uint16_t *out;
    size_t n, nb;
    const char *msg;
    if (dp_format(pat, sizeof(pat) / sizeof(pat[0]), d->secs, d->nanos, &out, &n, &msg) != DP_OK) {
        return NULL;
    }
    return av_units_to_text(out, n, &nb);
}

/* --- extern methods ---------------------------------------------- */

static av_type *date_newDate(av_type *self, environment *env, av_list *args)
{
    (void)env;
    date_state *d = AUSSOM_ALLOC(sizeof(date_state));
    d->magic = DATE_MAGIC;
    d->secs = 0;   /* epoch default */
    d->nanos = 0;
    if (av_list_size(args) > 0) {
        av_type *arg = av_list_get(args, 0);
        if (arg->type != C_TYPE_NULL) {
            if (arg->type != C_TYPE_INT) {
                return date_exc("Date.newDate(): Expecting an int (milliseconds).");
            }
            int64_t m = av_numeric_int(arg);
            if (m < 0) instant_now(d);
            else instant_from_millis(d, m);
        }
    }
    av_object_set_extern_object((av_object *)self, d);
    return self;
}

/* Reads one field of the receiver at UTC. */
#define DATE_GET(fn, name, expr)                                              \
    static av_type *fn(av_type *self, environment *env, av_list *args)        \
    { (void)env; (void)args;                                                  \
      DECOMPOSE_OR_RETURN(self_date(self), name);                             \
      return &av_int_new(expr)->base.base; }

DATE_GET(date_getHours,        "getHours",        b.hour)
DATE_GET(date_getMinutes,      "getMinutes",      b.min)
DATE_GET(date_getSeconds,      "getSeconds",      b.sec)
DATE_GET(date_getYear,         "getYear",         b.year)
DATE_GET(date_getMonth,        "getMonth",        b.mon)
DATE_GET(date_getDayOfMonth,   "getDayOfMonth",   b.day)
DATE_GET(date_getDayOfWeek,    "getDayOfWeek",    iso_dow(b.epoch_day))
DATE_GET(date_getDayOfYear,    "getDayOfYear",    day_of_year(&b))
DATE_GET(date_getMilliseconds, "getMilliseconds", b.nanos / 1000000)

static av_type *date_getTime(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    int64_t ms;
    if (!instant_to_millis(self_date(self), &ms)) return long_overflow("getTime");
    return &av_int_new(ms)->base.base;
}

/* setHours / setMinutes / setSeconds: zdt.withX(v).toInstant(). */
static av_type *date_set_time_field(av_type *self, av_list *args, const char *fn,
                                    const char *field, const char *range, int hi,
                                    int which)
{
    int v;
    av_type *err = date_int_arg(args, fn, &v);
    if (err != NULL) return err;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, fn);
    if (!field_in_range(v, 0, hi)) return field_range_exc(fn, field, range, v);
    if (which == 0) b.hour = v;
    else if (which == 1) b.min = v;
    else b.sec = v;
    recompose(&b, d);
    return self;
}

static av_type *date_setHours(av_type *self, environment *env, av_list *args)
{ (void)env; return date_set_time_field(self, args, "setHours", "HourOfDay", "0 - 23", 23, 0); }

static av_type *date_setMinutes(av_type *self, environment *env, av_list *args)
{ (void)env; return date_set_time_field(self, args, "setMinutes", "MinuteOfHour", "0 - 59", 59, 1); }

static av_type *date_setSeconds(av_type *self, environment *env, av_list *args)
{ (void)env; return date_set_time_field(self, args, "setSeconds", "SecondOfMinute", "0 - 59", 59, 2); }

static av_type *date_setTime(av_type *self, environment *env, av_list *args)
{
    (void)env;
    instant_from_millis(self_date(self), av_numeric_int(av_list_get(args, 0)));
    return self;
}

static av_type *date_toString(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_type *err = NULL;
    char *s = date_iso(self_date(self), "toString", &err);
    if (s == NULL) return (err != NULL) ? err : date_exc("Date.toString(): out of memory.");
    return &av_string_new(s)->base.base;
}

/* The units of string argument i, or NULL when it is not a string. */
static const uint16_t *string_arg(av_list *args, size_t i, size_t *n)
{
    av_type *v = av_list_get(args, i);
    if (v == NULL || v->type != C_TYPE_STRING) return NULL;
    return av_string_units((av_string *)v, n);
}

static av_type *date_parse(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t nt, np;
    const uint16_t *text = string_arg(args, 0, &nt);
    if (text == NULL) return date_exc("date_parse: argument 1 must be a string.");
    const uint16_t *pat = string_arg(args, 1, &np);
    if (pat == NULL) return date_exc("date_parse: argument 2 must be a string.");
    int64_t secs;
    int32_t nanos;
    const char *msg;
    dp_result r = dp_parse(text, nt, pat, np, &secs, &nanos, &msg);
    if (r == DP_THROWN) return &av_exception_new_thrown(msg)->base.base;
    if (r == DP_PARSE_ERROR) {
        /* ADate adds the DateTimeParseException's message. */
        size_t n = strlen(msg) + 64;
        char *m = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(m, n, "Date.parse(): Parse exception. (%s)", msg);
        return date_exc(m);
    }
    date_state *d = self_date(self);
    d->secs = secs;
    d->nanos = nanos;
    return self;
}

static av_type *date_format(av_type *self, environment *env, av_list *args)
{
    (void)env;
    static const uint16_t def[] = { 'y','y','y','y','-','M','M','-','d','d',' ','H','H',':',
                                    'm','m',':','s','s','.','S','S','S',' ','Z' };
    size_t np = sizeof(def) / sizeof(def[0]);
    const uint16_t *pat = string_arg(args, 0, &np);
    if (pat == NULL) {
        pat = def;
        np = sizeof(def) / sizeof(def[0]);
    }
    date_state *d = self_date(self);
    uint16_t *out;
    size_t n;
    const char *msg;
    if (dp_format(pat, np, d->secs, d->nanos, &out, &n, &msg) != DP_OK) {
        return &av_exception_new_thrown(msg)->base.base;
    }
    return &av_string_new_units(out, n)->base.base;
}

/* toEpochMilli() == 0, so a date less than a millisecond past the epoch
 * counts too. */
static av_type *date_isEpoch(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    int64_t ms;
    if (!instant_to_millis(self_date(self), &ms)) return long_overflow("isEpoch");
    return &av_bool_new(ms == 0)->base.base;
}

static av_type *date_toJson(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    av_type *err = NULL;
    char *s = date_iso(self_date(self), "toJson", &err);
    if (s == NULL) return (err != NULL) ? err : date_exc("Date.toJson(): out of memory.");
    return &av_string_new(s)->base.base;
}

/* IsoFields.WEEK_OF_WEEK_BASED_YEAR: week 1 is the one containing the
 * first Thursday, so a date's week number is derived from the Thursday
 * of its own week. */
static av_type *date_getWeekOfYear(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    DECOMPOSE_OR_RETURN(self_date(self), "getWeekOfYear");
    int64_t thursday = b.epoch_day - (iso_dow(b.epoch_day) - 1) + 3;
    int64_t wyear;
    int wm, wd;
    date_civil_from_days(thursday, &wyear, &wm, &wd);
    int week = (int)((thursday - date_days_from_civil(wyear, 1, 1)) / 7) + 1;
    return &av_int_new(week)->base.base;
}

/* --- component setters (mutate in place) -------------------------
 * Like Java's withYear / withMonth, the day is clamped to the target
 * month's length (2024-01-31 setMonth(2) -> 2024-02-29). */

static av_type *date_setYear(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int y;
    av_type *err = date_int_arg(args, "setYear", &y);
    if (err != NULL) return err;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "setYear");
    if (!field_in_range(y, YEAR_MIN, YEAR_MAX)) {
        return field_range_exc("setYear", "Year", "-999999999 - 999999999", y);
    }
    b.year = y;
    if (b.day > date_days_in_month(b.year, b.mon)) b.day = date_days_in_month(b.year, b.mon);
    recompose(&b, d);
    return self;
}

static av_type *date_setMonth(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int m;
    av_type *err = date_int_arg(args, "setMonth", &m);
    if (err != NULL) return err;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "setMonth");
    if (!field_in_range(m, 1, 12)) return field_range_exc("setMonth", "MonthOfYear", "1 - 12", m);
    b.mon = m;
    if (b.day > date_days_in_month(b.year, b.mon)) b.day = date_days_in_month(b.year, b.mon);
    recompose(&b, d);
    return self;
}

static av_type *date_setDayOfMonth(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int day;
    av_type *err = date_int_arg(args, "setDayOfMonth", &day);
    if (err != NULL) return err;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "setDayOfMonth");
    if (!field_in_range(day, 1, 31)) {
        return field_range_exc("setDayOfMonth", "DayOfMonth", "1 - 28/31", day);
    }
    if (day > date_days_in_month(b.year, b.mon)) {
        /* LocalDate's own two messages for a day the month lacks. */
        static const char *const months[] = {
            "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
            "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
        };
        char msg[160];
        if (b.mon == 2 && day == 29) {
            snprintf(msg, sizeof(msg), "Invalid date "
                     "'February 29' as '%lld' is not a leap year", (long long)b.year);
        } else {
            snprintf(msg, sizeof(msg), "Invalid date '%s %d'",
                     months[b.mon - 1], day);
        }
        return date_fn_exc("setDayOfMonth", msg);
    }
    b.day = day;
    recompose(&b, d);
    return self;
}

/* --- arithmetic (mutate in place; a negative amount subtracts) ---- */

/* LocalDate.resolvePreviousValid after plusYears / plusMonths: checks
 * the year Java computed in a long, clamps the day, and stores. */
static av_type *store_year_month(av_type *self, date_state *d, broken *b,
                                 int64_t year, int mon, const char *fn)
{
    if (year < YEAR_MIN || year > YEAR_MAX) {
        return field_value_exc(fn, "Year", "-999999999 - 999999999", year);
    }
    b->year = year;
    b->mon = mon;
    if (b->day > date_days_in_month(b->year, b->mon)) b->day = date_days_in_month(b->year, b->mon);
    recompose(b, d);
    return self;
}

/* zdt.plusYears(n): the new year is year + n in Java long arithmetic,
 * which wraps. */
static av_type *date_addYears(av_type *self, environment *env, av_list *args)
{
    (void)env;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "addYears");
    int64_t n = av_numeric_int(av_list_get(args, 0));
    if (n == 0) return self;
    return store_year_month(self, d, &b, wrap_add(b.year, n), b.mon, "addYears");
}

/* zdt.plusMonths(n): counts months from year 0, again in wrapping long
 * arithmetic. */
static av_type *date_addMonths(av_type *self, environment *env, av_list *args)
{
    (void)env;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "addMonths");
    int64_t n = av_numeric_int(av_list_get(args, 0));
    if (n == 0) return self;
    int64_t calc = wrap_add(b.year * 12 + (b.mon - 1), n);
    return store_year_month(self, d, &b, date_floordiv(calc, 12), (int)date_floormod(calc, 12) + 1,
                            "addMonths");
}

/* Instant.plus(n, DAYS / HOURS / MINUTES): plusSeconds(multiplyExact(n,
 * unit seconds)). */
#define DATE_ADD_SECONDS(name, scale)                                         \
    static av_type *date_##name(av_type *self, environment *env, av_list *args) \
    { (void)env; int64_t secs;                                                \
      if (__builtin_mul_overflow(av_numeric_int(av_list_get(args, 0)),        \
                                 (int64_t)(scale), &secs))                    \
          return long_overflow(#name);                                        \
      av_type *e = instant_plus(self_date(self), secs, 0, #name);             \
      return (e != NULL) ? e : self; }

DATE_ADD_SECONDS(addDays,    SECONDS_PER_DAY)
DATE_ADD_SECONDS(addHours,   3600LL)
DATE_ADD_SECONDS(addMinutes, 60LL)
DATE_ADD_SECONDS(addSeconds, 1LL)

/* Instant.plusMillis(n): plus(n / 1000, (n % 1000) * 1,000,000). */
static av_type *date_addMillis(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t n = av_numeric_int(av_list_get(args, 0));
    av_type *e = instant_plus(self_date(self), n / 1000, (n % 1000) * 1000000, "addMillis");
    return (e != NULL) ? e : self;
}

/* --- comparison and difference ------------------------------------ */

/* The Date passed as an argument. */
static date_state *other_date(av_type *arg)
{
    if (arg == NULL || arg->type != C_TYPE_OBJECT) return NULL;
    void *eo = av_object_get_extern_object((av_object *)arg);
    /* Must be a real Date, not just any object carrying state (L1). */
    if (eo == NULL || eo == (void *)arg ||
        ((date_state *)eo)->magic != DATE_MAGIC) return NULL;
    return (date_state *)eo;
}

/* ADate.otherInstant casts the argument's extern object to ADate; for
 * anything else that cast throws. */
static av_type *not_a_date(av_type *arg)
{
    const char *cls;
    switch (arg->type) {
    case C_TYPE_BOOL:   cls = "com.aussom.types.AussomBool"; break;
    case C_TYPE_INT:    cls = "com.aussom.types.AussomInt"; break;
    case C_TYPE_DOUBLE: cls = "com.aussom.types.AussomDouble"; break;
    case C_TYPE_STRING: cls = "com.aussom.types.AussomString"; break;
    case C_TYPE_LIST:   cls = "com.aussom.types.AussomList"; break;
    case C_TYPE_MAP:    cls = "com.aussom.types.AussomMap"; break;
    case C_TYPE_NULL:   cls = "com.aussom.types.AussomNull"; break;
    case C_TYPE_CALLBACK: cls = "com.aussom.types.AussomCallback"; break;
    default:            cls = "com.aussom.types.AussomObject"; break;
    }
    char msg[192];
    snprintf(msg, sizeof(msg), "java.lang.ClassCastException: class %s cannot be "
             "cast to class com.aussom.stdlib.ADate", cls);
    return &av_exception_new_thrown(msg)->base.base;
}

#define DATE_CMP(fn, op)                                                      \
    static av_type *fn(av_type *self, environment *env, av_list *args)        \
    { (void)env; date_state *o = other_date(av_list_get(args, 0));            \
      if (o == NULL) return not_a_date(av_list_get(args, 0));                 \
      return &av_bool_new(instant_compare(self_date(self), o) op 0)->base.base; }

DATE_CMP(date_isBefore, <)
DATE_CMP(date_isAfter,  >)
DATE_CMP(date_isSame,   ==)

static av_type *date_compare(av_type *self, environment *env, av_list *args)
{
    (void)env;
    date_state *o = other_date(av_list_get(args, 0));
    if (o == NULL) return not_a_date(av_list_get(args, 0));
    return &av_int_new(instant_compare(self_date(self), o))->base.base;
}

/* between(Other, Unit): ChronoUnit.between, which for two UTC dates is
 * LocalDateTime.until. Positive when Other is later. Time units count
 * whole units of the exact difference; days, weeks, months and years
 * count calendar units, each cut toward zero. */
static av_type *date_between(av_type *self, environment *env, av_list *args)
{
    (void)env;
    date_state *o = other_date(av_list_get(args, 0));
    if (o == NULL) return not_a_date(av_list_get(args, 0));
    const char *unit = "millis";
    if (av_list_size(args) > 1) {
        av_type *u = av_list_get(args, 1);
        if (u->type == C_TYPE_STRING) unit = av_string_get((av_string *)u);
        else if (u->type != C_TYPE_NULL) {
            return date_exc("Date.between(): Unit must be a string.");
        }
    }

    /* Nanoseconds per unit for the time units; 0 for the date units. */
    int64_t unit_nanos;
    int64_t per_day;
    if (strcmp(unit, "millis") == 0)       { unit_nanos = 1000000LL;             per_day = 86400000LL; }
    else if (strcmp(unit, "seconds") == 0) { unit_nanos = NANOS_PER_SECOND;      per_day = 86400LL; }
    else if (strcmp(unit, "minutes") == 0) { unit_nanos = 60 * NANOS_PER_SECOND; per_day = 1440LL; }
    else if (strcmp(unit, "hours") == 0)   { unit_nanos = 3600 * NANOS_PER_SECOND; per_day = 24LL; }
    else if (strcmp(unit, "days") == 0 || strcmp(unit, "weeks") == 0 ||
             strcmp(unit, "months") == 0 || strcmp(unit, "years") == 0) {
        unit_nanos = 0; per_day = 0;
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Date.between(): unknown unit '%s'. Allowed units are "
                 "millis, seconds, minutes, hours, days, weeks, months, years.", unit);
        return date_exc(msg);
    }

    broken a, e;
    av_type *err = decompose(self_date(self), &a, "between");
    if (err == NULL) err = decompose(o, &e, "between");
    if (err != NULL) return err;
    int64_t a_nod = ((int64_t)a.hour * 3600 + a.min * 60 + a.sec) * NANOS_PER_SECOND + a.nanos;
    int64_t e_nod = ((int64_t)e.hour * 3600 + e.min * 60 + e.sec) * NANOS_PER_SECOND + e.nanos;

    if (unit_nanos != 0) {
        int64_t amount = e.epoch_day - a.epoch_day;
        int64_t time_part = e_nod - a_nod;
        if (amount == 0) return &av_int_new(time_part / unit_nanos)->base.base;
        if (amount > 0) { amount--; time_part += NANOS_PER_DAY; }
        else { amount++; time_part -= NANOS_PER_DAY; }
        int64_t total;
        if (__builtin_mul_overflow(amount, per_day, &amount) ||
            __builtin_add_overflow(amount, time_part / unit_nanos, &total)) {
            return long_overflow("between");
        }
        return &av_int_new(total)->base.base;
    }

    /* Date units: step the end date back (or forward) a day when its
     * time of day has not reached the start's, then compare dates. */
    int64_t end_day = e.epoch_day;
    if (end_day > a.epoch_day && e_nod < a_nod) end_day--;
    else if (end_day < a.epoch_day && e_nod > a_nod) end_day++;
    int64_t days = end_day - a.epoch_day;
    if (strcmp(unit, "days") == 0) return &av_int_new(days)->base.base;
    if (strcmp(unit, "weeks") == 0) return &av_int_new(days / 7)->base.base;
    int64_t ey;
    int em, ed;
    date_civil_from_days(end_day, &ey, &em, &ed);
    int64_t packed1 = (a.year * 12 + (a.mon - 1)) * 32 + a.day;
    int64_t packed2 = (ey * 12 + (em - 1)) * 32 + ed;
    int64_t months = (packed2 - packed1) / 32;
    if (strcmp(unit, "years") == 0) return &av_int_new(months / 12)->base.base;
    return &av_int_new(months)->base.base;
}

/* --- current time and copying ------------------------------------- */

static av_type *date_now(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    instant_now(self_date(self));
    return self;
}

static av_type *date_copy(av_type *self, environment *env, av_list *args)
{
    (void)args;
    ast_class *cls = engine_resolve_class(environment_engine(env), "Date");
    if (cls == NULL) return date_exc("Date.copy(): Class 'Date' not found.");
    av_type *inst = class_instantiate(env, cls, false, av_list_new());
    if (av_is_ex(inst)) return inst;
    date_state *src = self_date(self), *dst = self_date(inst);
    dst->secs = src->secs;
    dst->nanos = src->nanos;
    return inst;
}

/* --- day boundaries ----------------------------------------------- */

static av_type *date_startOfDay(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "startOfDay");
    b.hour = 0; b.min = 0; b.sec = 0; b.nanos = 0;
    recompose(&b, d);
    return self;
}

static av_type *date_endOfDay(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    date_state *d = self_date(self);
    DECOMPOSE_OR_RETURN(d, "endOfDay");
    b.hour = 23; b.min = 59; b.sec = 59; b.nanos = 999000000;
    recompose(&b, d);
    return self;
}

/* --- display and calendar helpers ---------------------------------
 * The names match Java's Month / DayOfWeek enum toString: uppercase. */

static av_type *date_getMonthName(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    static const char *const names[] = {
        "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
        "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
    };
    DECOMPOSE_OR_RETURN(self_date(self), "getMonthName");
    return &av_string_new(names[b.mon - 1])->base.base;
}

static av_type *date_getDayOfWeekName(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    static const char *const names[] = {
        "MONDAY","TUESDAY","WEDNESDAY","THURSDAY","FRIDAY","SATURDAY","SUNDAY"
    };
    DECOMPOSE_OR_RETURN(self_date(self), "getDayOfWeekName");
    return &av_string_new(names[iso_dow(b.epoch_day) - 1])->base.base;
}

static av_type *date_isLeapYear(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    DECOMPOSE_OR_RETURN(self_date(self), "isLeapYear");
    return &av_bool_new(date_is_leap(b.year))->base.base;
}

static av_type *date_daysInMonth(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    DECOMPOSE_OR_RETURN(self_date(self), "daysInMonth");
    return &av_int_new(date_days_in_month(b.year, b.mon))->base.base;
}

/* --- helpers for json pack / unpack ------------------------------ */

/* The ISO string json.pack writes, or NULL when the date is past what
 * toString can show (Java's pack throws there too). */
const char *av_date_iso_str(av_type *date_obj)
{
    return date_iso(self_date(date_obj), "pack", NULL);
}

av_type *av_date_from_iso(environment *env, const char *iso)
{
    ast_class *cls = engine_resolve_class(environment_engine(env), "Date");
    /* AJson.getJsonDate throws both of its errors. */
    if (cls == NULL)
        return &av_exception_new_thrown_aussom("json.unpack(): Class 'Date' not found.")->base.base;
    date_state parsed;
    if (!parse_iso_offset(iso, &parsed)) {
        size_t n = strlen(iso) + 128;
        char *msg = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(msg, n, "json.unpack(): Getting value for key 'value'. Parse "
                 "exception of String (ISO 8601) with value '%s'.", iso);
        return &av_exception_new_thrown_aussom(msg)->base.base;
    }
    av_type *inst = class_instantiate(env, cls, false, av_list_new());
    if (av_is_ex(inst)) return inst;
    date_state *d = self_date(inst);
    d->secs = parsed.secs;
    d->nanos = parsed.nanos;
    return inst;
}

const aussom_extern_method stdlib_adate_methods[] = {
    { "newDate", date_newDate },
    { "getHours", date_getHours },
    { "getMinutes", date_getMinutes },
    { "getSeconds", date_getSeconds },
    { "getTime", date_getTime },
    { "setHours", date_setHours },
    { "setMinutes", date_setMinutes },
    { "setSeconds", date_setSeconds },
    { "setTime", date_setTime },
    { "toString", date_toString },
    { "parse", date_parse },
    { "format", date_format },
    { "isEpoch", date_isEpoch },
    { "toJson", date_toJson },
    { "getYear", date_getYear },
    { "getMonth", date_getMonth },
    { "getDayOfMonth", date_getDayOfMonth },
    { "getDayOfWeek", date_getDayOfWeek },
    { "getDayOfYear", date_getDayOfYear },
    { "getWeekOfYear", date_getWeekOfYear },
    { "getMilliseconds", date_getMilliseconds },
    { "setYear", date_setYear },
    { "setMonth", date_setMonth },
    { "setDayOfMonth", date_setDayOfMonth },
    { "addYears", date_addYears },
    { "addMonths", date_addMonths },
    { "addDays", date_addDays },
    { "addHours", date_addHours },
    { "addMinutes", date_addMinutes },
    { "addSeconds", date_addSeconds },
    { "addMillis", date_addMillis },
    { "isBefore", date_isBefore },
    { "isAfter", date_isAfter },
    { "isSame", date_isSame },
    { "compare", date_compare },
    { "between", date_between },
    { "now", date_now },
    { "copy", date_copy },
    { "startOfDay", date_startOfDay },
    { "endOfDay", date_endOfDay },
    { "getMonthName", date_getMonthName },
    { "getDayOfWeekName", date_getDayOfWeekName },
    { "isLeapYear", date_isLeapYear },
    { "daysInMonth", date_daysInMonth },
    { "pack", stdlib_pack },
    { NULL, NULL }
};
