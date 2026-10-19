/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Date.format and Date.parse patterns.
 *
 * Building blocks:
 *
 * - A pattern compiles to a list of elements: literal text, numbers,
 *   fractions, names, offsets, zones, day periods and optional
 *   sections. Each element can be padded with spaces to a width ('p').
 * - Formatting prints each element from the instant read at UTC.
 * - Parsing reads each element into a set of fields, then resolves the
 *   fields into a date, a time and an offset the way Java's SMART
 *   resolver does, and checks the fields left over against the result.
 *
 * Adjacent numbers. A number of variable width followed directly by
 * numbers of fixed width (such as yyyyMMdd) leaves room for them: when
 * parsing, it takes all the digits there are minus the widths of the
 * fixed numbers after it. A variable-width number after it cancels that,
 * and starts a new run of its own. Any other element ends the run.
 *
 * Java quirks copied here: a padded number followed by another number
 * throws ClassCastException when the pattern is compiled; a failed
 * padded field reports the sum of two positions as its error index; a
 * full GMT offset (OOOO) that runs off the end of the text reports
 * Java's out-of-bounds error.
 *
 * Zones. A pattern that parses a zone (VV, z, v) reads an offset, UTC,
 * GMT or UT with an optional offset, a region ID such as
 * America/New_York, or (z and v) a zone name such as PST. The region's
 * rules come from the time zone database (see tzdb.c).
 */

#include "date_pattern.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av_string.h"
#include "tzdb.h"

/* --- calendar ----------------------------------------------------- */

int64_t date_floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) q--;
    return q;
}

int64_t date_floormod(int64_t a, int64_t b)
{
    int64_t r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) r += b;
    return r;
}

/* Days since 1970-01-01 for y-m-d (Hinnant's days_from_civil). */
int64_t date_days_from_civil(int64_t y, int m, int d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* y-m-d for days since 1970-01-01 (Hinnant's civil_from_days). */
void date_civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

bool date_is_leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int date_days_in_month(int64_t y, int m)
{
    static const int dm[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && date_is_leap(y)) return 29;
    return dm[m - 1];
}

/* LocalDate's epoch-day range (years -999,999,999 to 999,999,999). */
#define EPOCH_DAY_MIN (-365243219162LL)
#define EPOCH_DAY_MAX (365241780471LL)
#define MJD_OFFSET 40587LL
#define NANOS_PER_SECOND 1000000000LL

/* ISO day of week, Monday = 1 .. Sunday = 7. */
static int iso_dow(int64_t epoch_day)
{
    return (int)date_floormod(epoch_day + 3, 7) + 1;
}

/* Day of week in WeekFields(SUNDAY, 1): Sunday = 1 .. Saturday = 7. */
static int local_dow(int iso)
{
    return iso % 7 + 1;
}

/* First day of week 1 of week-based year y: the Sunday on or before
 * January 1, since one day of the new year is enough. */
static int64_t wby_start(int64_t y)
{
    int64_t jan1 = date_days_from_civil(y, 1, 1);
    return jan1 - (local_dow(iso_dow(jan1)) - 1);
}

/* --- messages ----------------------------------------------------- */

static char *msgf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = AUSSOM_ALLOC_ATOMIC((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/* LocalDate.toString. */
static char *date_text(int64_t epoch_day)
{
    int64_t y;
    int m, d;
    date_civil_from_days(epoch_day, &y, &m, &d);
    if (y > 9999) return msgf("+%lld-%02d-%02d", (long long)y, m, d);
    if (y < 0) return msgf("-%04lld-%02d-%02d", (long long)-y, m, d);
    return msgf("%04lld-%02d-%02d", (long long)y, m, d);
}

/* LocalTime.toString. */
static char *time_text(int64_t nano_of_day)
{
    int64_t sod = nano_of_day / NANOS_PER_SECOND;
    int nano = (int)(nano_of_day % NANOS_PER_SECOND);
    int h = (int)(sod / 3600), mi = (int)(sod / 60 % 60), s = (int)(sod % 60);
    if (s == 0 && nano == 0) return msgf("%02d:%02d", h, mi);
    if (nano == 0) return msgf("%02d:%02d:%02d", h, mi, s);
    if (nano % 1000000 == 0) return msgf("%02d:%02d:%02d.%03d", h, mi, s, nano / 1000000);
    if (nano % 1000 == 0) return msgf("%02d:%02d:%02d.%06d", h, mi, s, nano / 1000);
    return msgf("%02d:%02d:%02d.%09d", h, mi, s, nano);
}

/* --- fields ------------------------------------------------------- */

enum {
    F_ERA, F_YOE, F_YEAR, F_MONTH, F_DOM, F_DOY, F_DOW, F_AWOM, F_QOY, F_MJD,
    F_AMPM, F_CHOA, F_HOA, F_CHOD, F_HOUR, F_MIN, F_SEC, F_NANO, F_MOD, F_NOD,
    F_WBY, F_WOWBY, F_WOM, F_LDOW, F_OFFSET, F_MILLI, F_SOD,
    F_COUNT
};

typedef struct { const char *name; int64_t lo, hi; const char *range; } field_info;

#define WF "[WeekFields[SUNDAY,1]]"
static const field_info FIELDS[F_COUNT] = {
    { "Era", 0, 1, "0 - 1" },
    { "YearOfEra", 1, 1000000000LL, "1 - 999999999/1000000000" },
    { "Year", -999999999LL, 999999999LL, "-999999999 - 999999999" },
    { "MonthOfYear", 1, 12, "1 - 12" },
    { "DayOfMonth", 1, 31, "1 - 28/31" },
    { "DayOfYear", 1, 366, "1 - 365/366" },
    { "DayOfWeek", 1, 7, "1 - 7" },
    { "AlignedWeekOfMonth", 1, 5, "1 - 4/5" },
    { "QuarterOfYear", 1, 4, "1 - 4" },
    { "ModifiedJulianDay", -365243178575LL, 365241821058LL, "-365243178575 - 365241821058" },
    { "AmPmOfDay", 0, 1, "0 - 1" },
    { "ClockHourOfAmPm", 1, 12, "1 - 12" },
    { "HourOfAmPm", 0, 11, "0 - 11" },
    { "ClockHourOfDay", 1, 24, "1 - 24" },
    { "HourOfDay", 0, 23, "0 - 23" },
    { "MinuteOfHour", 0, 59, "0 - 59" },
    { "SecondOfMinute", 0, 59, "0 - 59" },
    { "NanoOfSecond", 0, 999999999LL, "0 - 999999999" },
    { "MilliOfDay", 0, 86399999LL, "0 - 86399999" },
    { "NanoOfDay", 0, 86399999999999LL, "0 - 86399999999999" },
    { "WeekBasedYear" WF, -999999999LL, 999999999LL, "-999999999 - 999999999" },
    { "WeekOfWeekBasedYear" WF, 1, 53, "1 - 52/53" },
    { "WeekOfMonth" WF, 0, 6, "0/1 - 4/6" },
    { "DayOfWeek" WF, 1, 7, "1 - 7" },
    { "OffsetSeconds", -64800, 64800, "-64800 - 64800" },
    { "MilliOfSecond", 0, 999, "0 - 999" },
    { "SecondOfDay", 0, 86399, "0 - 86399" },
};

/* Every field of the instant at UTC, for formatting and cross-checks. */
static void fields_of(int64_t epoch_day, int64_t nano_of_day, int64_t *fv)
{
    int64_t y;
    int m, d;
    date_civil_from_days(epoch_day, &y, &m, &d);
    int dow = iso_dow(epoch_day);
    int64_t sod = nano_of_day / NANOS_PER_SECOND;
    int hour = (int)(sod / 3600);

    fv[F_ERA] = y >= 1 ? 1 : 0;
    fv[F_YOE] = y >= 1 ? y : 1 - y;
    fv[F_YEAR] = y;
    fv[F_MONTH] = m;
    fv[F_DOM] = d;
    fv[F_DOY] = epoch_day - date_days_from_civil(y, 1, 1) + 1;
    fv[F_DOW] = dow;
    fv[F_AWOM] = (d - 1) / 7 + 1;
    fv[F_QOY] = (m - 1) / 3 + 1;
    fv[F_MJD] = epoch_day + MJD_OFFSET;
    fv[F_AMPM] = hour / 12;
    fv[F_HOA] = hour % 12;
    fv[F_CHOA] = hour % 12 == 0 ? 12 : hour % 12;
    fv[F_CHOD] = hour == 0 ? 24 : hour;
    fv[F_HOUR] = hour;
    fv[F_MIN] = sod / 60 % 60;
    fv[F_SEC] = sod % 60;
    fv[F_NANO] = nano_of_day % NANOS_PER_SECOND;
    fv[F_MOD] = nano_of_day / 1000000;
    fv[F_NOD] = nano_of_day;
    fv[F_MILLI] = fv[F_NANO] / 1000000;
    fv[F_SOD] = sod;
    fv[F_OFFSET] = 0;

    /* Week fields, weeks starting on Sunday. */
    int64_t first = epoch_day - (d - 1);
    fv[F_LDOW] = local_dow(dow);
    fv[F_WOM] = (d - 1 + local_dow(iso_dow(first)) - 1) / 7 + 1;
    int64_t wy = y;
    if (y < 999999999LL && epoch_day >= wby_start(y + 1)) wy = y + 1;
    fv[F_WBY] = wy;
    fv[F_WOWBY] = (epoch_day - wby_start(wy)) / 7 + 1;
}

/* --- names (en_US) ------------------------------------------------ */

enum { ST_SHORT, ST_FULL, ST_NARROW };

static const char *const MONTH_NAMES[3][12] = {
    { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" },
    { "January","February","March","April","May","June","July","August",
      "September","October","November","December" },
    { "J","F","M","A","M","J","J","A","S","O","N","D" },
};
static const char *const DOW_NAMES[3][7] = {
    { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" },
    { "Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday" },
    { "M","T","W","T","F","S","S" },
};
static const char *const ERA_NAMES[3][2] = {
    { "BC", "AD" }, { "Before Christ", "Anno Domini" }, { "B", "A" },
};
static const char *const AMPM_NAMES[2] = { "AM", "PM" };
static const char *const QUARTER_NAMES[3][4] = {
    { "Q1","Q2","Q3","Q4" },
    { "1st quarter","2nd quarter","3rd quarter","4th quarter" },
    { "1","2","3","4" },
};

/* Day periods (B): midnight, at night, in the morning, noon, in the
 * afternoon, in the evening, and the two halves of the day, which are
 * only parsed. Each is a span of minutes of the day; midnight and noon
 * are a single minute. */
enum { DP_MIDNIGHT, DP_NIGHT, DP_MORNING, DP_NOON, DP_AFTERNOON, DP_EVENING, DP_AM, DP_PM,
       DP_PERIODS };
static const char *const PERIOD_NAMES[3][DP_PERIODS] = {
    { "midnight","at night","in the morning","noon","in the afternoon","in the evening","AM","PM" },
    { "midnight","at night","in the morning","noon","in the afternoon","in the evening","AM","PM" },
    { "mi","at night","in the morning","n","in the afternoon","in the evening","a","p" },
};
static const int PERIOD_FROM[DP_PERIODS] = { 0, 1260, 360, 720, 720, 1080, 0, 720 };
static const int PERIOD_TO[DP_PERIODS]   = { 0, 360, 720, 720, 1080, 1260, 720, 1440 };

static int period_of_minute(int m)
{
    if (m == 0) return DP_MIDNIGHT;
    if (m < 360) return DP_NIGHT;
    if (m < 720) return DP_MORNING;
    if (m == 720) return DP_NOON;
    if (m < 1080) return DP_AFTERNOON;
    if (m < 1260) return DP_EVENING;
    return DP_NIGHT;
}

static bool period_has_minute(int p, int m)
{
    int from = PERIOD_FROM[p], to = PERIOD_TO[p];
    if (from == to) return m == from;
    if (from < to) return m >= from && m < to;
    return m >= from || m < to;
}

/* The names of a text field in one style, by value. */
typedef struct { const char *const *names; int count; int first; } name_set;

static name_set names_for(int field, int style)
{
    name_set s = { NULL, 0, 0 };
    switch (field) {
    case F_MONTH: s.names = MONTH_NAMES[style]; s.count = 12; s.first = 1; break;
    case F_DOW:   s.names = DOW_NAMES[style];   s.count = 7;  s.first = 1; break;
    case F_ERA:   s.names = ERA_NAMES[style];   s.count = 2;  s.first = 0; break;
    case F_AMPM:  s.names = AMPM_NAMES;         s.count = 2;  s.first = 0; break;
    case F_QOY:   s.names = QUARTER_NAMES[style]; s.count = 4; s.first = 1; break;
    default: break;
    }
    return s;
}

/* --- pattern elements --------------------------------------------- */

typedef enum {
    EK_LIT, EK_NUM, EK_REDUCED, EK_FRAC, EK_TEXT, EK_PERIOD, EK_OFFSET,
    EK_LOCOFF, EK_ZONEID, EK_ZONETEXT, EK_OPT, EK_DEFAULTS
} elem_kind;

enum { SS_NORMAL, SS_NOT_NEG, SS_EXCEEDS };

/* Offset layouts, as X / XX / XXX / XXXX / XXXXX. */
enum { OF_HHmm, OF_HHMM, OF_HH_MM, OF_HHMMss, OF_HH_MM_ss };

typedef struct elem {
    elem_kind kind;
    int field;
    int minw, maxw, sign;
    int reserve;              /* digits the fixed-width numbers after it take */
    int style;                /* names, offsets, zone text: which form */
    const char *notext;       /* offset: the text for a zero offset */
    uint16_t *lit;            /* literal text */
    size_t nlit;
    int pad;                  /* > 0: padded with spaces to this width */
    struct elem *kids;        /* optional section */
    size_t nkids;
} elem;

typedef struct {
    elem *v;
    size_t n, cap;
    long head;                /* the number leading a run of adjacent numbers, or -1 */
} elem_list;

static elem *list_add(elem_list *l)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        elem *nv = AUSSOM_ALLOC(cap * sizeof(elem));
        if (l->n) memcpy(nv, l->v, l->n * sizeof(elem));
        l->v = nv;
        l->cap = cap;
    }
    elem *e = &l->v[l->n++];
    memset(e, 0, sizeof(*e));
    return e;
}

#define CCE_TEXT "java.lang.ClassCastException: class java.time.format." \
    "DateTimeFormatterBuilder$PadPrinterParserDecorator cannot be cast to class " \
    "java.time.format.DateTimeFormatterBuilder$NumberPrinterParser " \
    "(java.time.format.DateTimeFormatterBuilder$PadPrinterParserDecorator and " \
    "java.time.format.DateTimeFormatterBuilder$NumberPrinterParser are in module " \
    "java.base of loader 'bootstrap')"
#define IAE "java.lang.IllegalArgumentException: "

typedef struct {
    const uint16_t *p;
    size_t n;
    elem_list *stack[64];
    int depth;
    int pad;                  /* pending pad width */
    const char *err;
} compiler;

static bool is_letter(uint16_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* The whole pattern, for messages that quote it. */
static char *pattern_text(const compiler *c)
{
    size_t nb;
    return av_units_to_text(c->p, c->n, &nb);
}

/* Appends a non-number element: it ends any run of adjacent numbers. */
static elem *add_plain(compiler *c, elem_kind kind)
{
    elem_list *l = c->stack[c->depth];
    elem *e = list_add(l);
    e->kind = kind;
    e->pad = c->pad;
    c->pad = 0;
    l->head = -1;
    return e;
}

/* Appends a number, a reduced number or a fraction, keeping track of
 * the run of adjacent numbers it belongs to. */
static void add_number(compiler *c, elem_kind kind, int field, int minw, int maxw, int sign)
{
    elem_list *l = c->stack[c->depth];
    bool fixed = kind != EK_NUM || (minw == maxw && sign == SS_NOT_NEG);
    int pad = c->pad;
    c->pad = 0;
    if (pad == 0 && l->head >= 0 && l->v[l->head].pad > 0) {
        c->err = CCE_TEXT;
        return;
    }
    long idx = (long)l->n;
    elem *e = list_add(l);
    e->kind = kind;
    e->field = field;
    e->minw = minw;
    e->maxw = maxw;
    e->sign = sign;
    e->pad = pad;
    if (pad > 0 || l->head < 0) {
        l->head = idx;
    } else if (fixed) {
        l->v[l->head].reserve += e->maxw;
    } else {
        l->v[l->head].reserve = 0;
        l->head = idx;
    }
}

static void add_literal(compiler *c, const uint16_t *s, size_t n)
{
    elem *e = add_plain(c, EK_LIT);
    e->lit = AUSSOM_ALLOC_ATOMIC((n ? n : 1) * sizeof(uint16_t));
    memcpy(e->lit, s, n * sizeof(uint16_t));
    e->nlit = n;
}

static void too_many(compiler *c, uint16_t letter)
{
    c->err = msgf(IAE "Too many pattern letters: %c", (char)letter);
}

static void add_text(compiler *c, int field, int count)
{
    elem *e = add_plain(c, EK_TEXT);
    e->field = field;
    e->style = count == 4 ? ST_FULL : count == 5 ? ST_NARROW : ST_SHORT;
}

/* Value (1), two digits (2), short (3), full (4) or narrow (5) name. */
static void add_value_or_text(compiler *c, uint16_t letter, int field, int count)
{
    if (count == 1) add_number(c, EK_NUM, field, 1, 19, SS_NORMAL);
    else if (count == 2) add_number(c, EK_NUM, field, 2, 2, SS_NOT_NEG);
    else if (count <= 5) add_text(c, field, count);
    else too_many(c, letter);
}

static void add_year(compiler *c, int field, int count)
{
    if (count == 2) add_number(c, EK_REDUCED, field, 2, 2, SS_NOT_NEG);
    else if (count == 1) add_number(c, EK_NUM, field, 1, 19, SS_NORMAL);
    else if (count == 3) add_number(c, EK_NUM, field, 3, 19, SS_NORMAL);
    else if (field == F_WBY) add_number(c, EK_NUM, field, count, count > 19 ? count : 19, SS_EXCEEDS);
    else if (count > 19) c->err = msgf(IAE "The minimum width must be from 1 to 19 inclusive but was %d", count);
    else add_number(c, EK_NUM, field, count, 19, SS_EXCEEDS);
}

/* A wide number such as n, N, A or g: at least `count` digits. */
static void add_wide(compiler *c, int field, int count, int sign)
{
    if (count > 19) c->err = msgf(IAE "The minimum width must be from 1 to 19 inclusive but was %d", count);
    else add_number(c, EK_NUM, field, count, 19, sign);
}

static void add_offset(compiler *c, int layout, const char *notext)
{
    elem *e = add_plain(c, EK_OFFSET);
    e->style = layout;
    e->notext = notext;
}

static void add_field(compiler *c, uint16_t letter, int count)
{
    switch (letter) {
    case 'G':
        if (count <= 5) add_text(c, F_ERA, count); else too_many(c, letter);
        break;
    case 'u': add_year(c, F_YEAR, count); break;
    case 'y': add_year(c, F_YOE, count); break;
    case 'Y': add_year(c, F_WBY, count); break;
    case 'Q': case 'q': add_value_or_text(c, letter, F_QOY, count); break;
    case 'M': case 'L': add_value_or_text(c, letter, F_MONTH, count); break;
    case 'w':
        if (count == 1) add_number(c, EK_NUM, F_WOWBY, 1, 2, SS_NOT_NEG);
        else if (count == 2) add_number(c, EK_NUM, F_WOWBY, 2, 2, SS_NOT_NEG);
        else too_many(c, letter);
        break;
    case 'W':
        if (count == 1) add_number(c, EK_NUM, F_WOM, 1, 1, SS_NOT_NEG); else too_many(c, letter);
        break;
    case 'E':
        if (count <= 5) add_text(c, F_DOW, count < 4 ? 3 : count); else too_many(c, letter);
        break;
    case 'e':
        if (count == 1) add_number(c, EK_NUM, F_LDOW, 1, 1, SS_NOT_NEG);
        else if (count == 2) add_number(c, EK_NUM, F_LDOW, 2, 2, SS_NOT_NEG);
        else if (count <= 5) add_text(c, F_DOW, count);
        else too_many(c, letter);
        break;
    case 'c':
        if (count == 1) add_number(c, EK_NUM, F_LDOW, 1, 1, SS_NOT_NEG);
        else if (count == 2) c->err = IAE "Invalid pattern \"cc\"";
        else if (count <= 5) add_text(c, F_DOW, count);
        else too_many(c, letter);
        break;
    case 'a':
        if (count == 1) add_text(c, F_AMPM, 3); else too_many(c, letter);
        break;
    case 'B':
        if (count == 1 || count == 4 || count == 5) {
            elem *e = add_plain(c, EK_PERIOD);
            e->style = count == 1 ? ST_SHORT : count == 4 ? ST_FULL : ST_NARROW;
        } else {
            c->err = IAE "Wrong number of pattern letters: B";
        }
        break;
    case 'h': case 'K': case 'k': case 'H': case 'm': case 's': case 'd': {
        int f = letter == 'h' ? F_CHOA : letter == 'K' ? F_HOA : letter == 'k' ? F_CHOD :
                letter == 'H' ? F_HOUR : letter == 'm' ? F_MIN : letter == 's' ? F_SEC : F_DOM;
        if (count == 1) add_number(c, EK_NUM, f, 1, 19, SS_NORMAL);
        else if (count == 2) add_number(c, EK_NUM, f, 2, 2, SS_NOT_NEG);
        else too_many(c, letter);
        break;
    }
    case 'D':
        if (count == 1) add_number(c, EK_NUM, F_DOY, 1, 19, SS_NORMAL);
        else if (count == 2) add_number(c, EK_NUM, F_DOY, 2, 3, SS_NOT_NEG);
        else if (count == 3) add_number(c, EK_NUM, F_DOY, 3, 3, SS_NOT_NEG);
        else too_many(c, letter);
        break;
    case 'F':
        if (count == 1) add_number(c, EK_NUM, F_AWOM, 1, 19, SS_NORMAL); else too_many(c, letter);
        break;
    case 'g': add_wide(c, F_MJD, count, SS_NORMAL); break;
    case 'n': add_wide(c, F_NANO, count, SS_NOT_NEG); break;
    case 'N': add_wide(c, F_NOD, count, SS_NOT_NEG); break;
    case 'A': add_wide(c, F_MOD, count, SS_NOT_NEG); break;
    case 'S':
        if (count > 9) c->err = msgf(IAE "Minimum width must be from 0 to 9 inclusive but was %d", count);
        else add_number(c, EK_FRAC, F_NANO, count, count, SS_NOT_NEG);
        break;
    case 'V':
        if (count == 2) add_plain(c, EK_ZONEID);
        else c->err = IAE "Pattern letter count must be 2: V";
        break;
    case 'v':
        if (count == 1 || count == 4) add_plain(c, EK_ZONETEXT)->style = count;
        else c->err = IAE "Wrong number of pattern letters: v";
        break;
    case 'z':
        if (count <= 4) add_plain(c, EK_ZONETEXT)->style = count; else too_many(c, letter);
        break;
    case 'O':
        if (count == 1 || count == 4) add_plain(c, EK_LOCOFF)->style = count == 1 ? ST_SHORT : ST_FULL;
        else c->err = IAE "Pattern letter count must be 1 or 4: O";
        break;
    case 'X': {
        if (count <= 5) add_offset(c, count - 1, "Z"); else too_many(c, letter);
        break;
    }
    case 'x': {
        static const char *const zero[] = { "+00", "+0000", "+00:00", "+0000", "+00:00" };
        if (count <= 5) add_offset(c, count - 1, zero[count - 1]); else too_many(c, letter);
        break;
    }
    case 'Z':
        if (count <= 3) add_offset(c, OF_HHMM, "+0000");
        else if (count == 4) add_plain(c, EK_LOCOFF)->style = ST_FULL;
        else if (count == 5) add_offset(c, OF_HH_MM_ss, "Z");
        else too_many(c, letter);
        break;
    default:
        c->err = msgf(IAE "Unknown pattern letter: %c", (char)letter);
        break;
    }
}

/* Closes the innermost optional section into its parent. */
static void close_optional(compiler *c)
{
    elem_list *kid = c->stack[c->depth--];
    elem *e = add_plain(c, EK_OPT);
    e->kids = kid->v;
    e->nkids = kid->n;
}

/* DateTimeFormatterBuilder.appendPattern. NULL with *err set to the
 * thrown exception when the pattern is not valid. With `defaults`, the
 * pattern is followed by ADate's parseDefaulting calls. ADate calls
 * toFormatter() first, which closes any optional section the pattern
 * leaves open, so the defaults land outside it and always apply. */
static elem_list *compile(const uint16_t *p, size_t n, bool defaults, const char **err)
{
    compiler c;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.n = n;
    c.stack[0] = AUSSOM_ALLOC(sizeof(elem_list));
    c.stack[0]->head = -1;

    size_t i = 0;
    while (i < n && c.err == NULL) {
        uint16_t ch = p[i];
        if (is_letter(ch)) {
            size_t j = i;
            while (j < n && p[j] == ch) j++;
            int count = (int)(j - i);
            if (ch == 'p') {
                if (j < n && is_letter(p[j])) {
                    c.pad = count;
                    ch = p[j];
                    i = j;
                    while (j < n && p[j] == ch) j++;
                    count = (int)(j - i);
                } else {
                    c.err = msgf(IAE "Pad letter 'p' must be followed by valid pad pattern: %s",
                                 pattern_text(&c));
                    break;
                }
            }
            add_field(&c, ch, count);
            i = j;
        } else if (ch == '\'') {
            size_t j = i + 1;
            for (;;) {
                if (j >= n) {
                    c.err = msgf(IAE "Pattern ends with an incomplete string literal: %s",
                                 pattern_text(&c));
                    break;
                }
                if (p[j] == '\'') {
                    if (j + 1 < n && p[j + 1] == '\'') { j += 2; continue; }
                    break;
                }
                j++;
            }
            if (c.err != NULL) break;
            if (j == i + 1) {
                uint16_t q = '\'';
                add_literal(&c, &q, 1);
            } else {
                uint16_t *s = AUSSOM_ALLOC_ATOMIC((j - i) * sizeof(uint16_t));
                size_t k = 0;
                for (size_t x = i + 1; x < j; x++) {
                    s[k++] = p[x];
                    if (p[x] == '\'') x++;
                }
                add_literal(&c, s, k);
            }
            i = j + 1;
        } else if (ch == '[') {
            if (c.depth + 1 >= (int)(sizeof(c.stack) / sizeof(c.stack[0]))) {
                c.err = IAE "Pattern nests too many optional sections";
                break;
            }
            c.stack[c.depth]->head = -1;
            c.stack[++c.depth] = AUSSOM_ALLOC(sizeof(elem_list));
            c.stack[c.depth]->head = -1;
            i++;
        } else if (ch == ']') {
            if (c.depth == 0) {
                c.err = IAE "Pattern invalid as it contains ] without previous [";
                break;
            }
            close_optional(&c);
            i++;
        } else if (ch == '{' || ch == '}' || ch == '#') {
            c.err = msgf(IAE "Pattern includes reserved character: '%c'", (char)ch);
        } else {
            add_literal(&c, &ch, 1);
            i++;
        }
    }
    if (c.err != NULL) {
        *err = c.err;
        return NULL;
    }
    while (c.depth > 0) close_optional(&c);
    if (defaults) add_plain(&c, EK_DEFAULTS);
    return c.stack[0];
}

/* --- formatting --------------------------------------------------- */

typedef struct { uint16_t *u; size_t n, cap; } ubuf;

static void ub_put(ubuf *b, uint16_t c)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        uint16_t *nu = AUSSOM_ALLOC_ATOMIC(cap * sizeof(uint16_t));
        if (b->n) memcpy(nu, b->u, b->n * sizeof(uint16_t));
        b->u = nu;
        b->cap = cap;
    }
    b->u[b->n++] = c;
}

static void ub_puts(ubuf *b, const char *s)
{
    while (*s) ub_put(b, (uint8_t)*s++);
}

typedef struct {
    ubuf out;
    const int64_t *fv;
    int minute;               /* minute of the day, for day periods */
    const char *err;
} printer;

static void print_number(printer *pr, const elem *e)
{
    int64_t v = pr->fv[e->field];
    const field_info *fi = &FIELDS[e->field];
    if (e->kind == EK_REDUCED) {
        int64_t r = (v < 0 ? -v : v) % 100;
        ub_put(&pr->out, (uint16_t)('0' + r / 10));
        ub_put(&pr->out, (uint16_t)('0' + r % 10));
        return;
    }
    if (e->kind == EK_FRAC) {
        char nb[16];
        snprintf(nb, sizeof(nb), "%09lld", (long long)v);
        for (int i = 0; i < e->minw; i++) ub_put(&pr->out, (uint16_t)nb[i]);
        return;
    }
    char digits[24];
    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)mag);
    int nd = (int)strlen(digits);
    if (nd > e->maxw) {
        pr->err = msgf("java.time.DateTimeException: Field %s cannot be printed as the value %lld "
                       "exceeds the maximum print width of %d", fi->name, (long long)v, e->maxw);
        return;
    }
    if (v < 0) {
        if (e->sign == SS_NOT_NEG) {
            pr->err = msgf("java.time.DateTimeException: Field %s cannot be printed as the value %lld "
                           "cannot be negative according to the SignStyle", fi->name, (long long)v);
            return;
        }
        ub_put(&pr->out, '-');
    } else if (e->sign == SS_EXCEEDS && nd > e->minw) {
        ub_put(&pr->out, '+');
    }
    for (int i = nd; i < e->minw; i++) ub_put(&pr->out, '0');
    ub_puts(&pr->out, digits);
}

static void print_list(printer *pr, const elem *v, size_t n);

static void print_elem(printer *pr, const elem *e)
{
    switch (e->kind) {
    case EK_LIT:
        for (size_t i = 0; i < e->nlit; i++) ub_put(&pr->out, e->lit[i]);
        break;
    case EK_NUM: case EK_REDUCED: case EK_FRAC:
        print_number(pr, e);
        break;
    case EK_TEXT: {
        name_set s = names_for(e->field, e->style);
        ub_puts(&pr->out, s.names[pr->fv[e->field] - s.first]);
        break;
    }
    case EK_PERIOD:
        ub_puts(&pr->out, PERIOD_NAMES[e->style][period_of_minute(pr->minute)]);
        break;
    case EK_OFFSET:
        ub_puts(&pr->out, e->notext);
        break;
    case EK_LOCOFF:
        ub_puts(&pr->out, "GMT");
        break;
    case EK_ZONEID: case EK_ZONETEXT:
        ub_puts(&pr->out, "Z");
        break;
    case EK_OPT:
        print_list(pr, e->kids, e->nkids);
        break;
    case EK_DEFAULTS:
        break;
    }
}

static void print_list(printer *pr, const elem *v, size_t n)
{
    for (size_t i = 0; i < n && pr->err == NULL; i++) {
        size_t start = pr->out.n;
        print_elem(pr, &v[i]);
        if (pr->err != NULL || v[i].pad == 0) continue;
        size_t len = pr->out.n - start;
        if (len > (size_t)v[i].pad) {
            pr->err = msgf("java.time.DateTimeException: Cannot print as output of %zu characters "
                           "exceeds pad width of %d", len, v[i].pad);
            return;
        }
        size_t add = (size_t)v[i].pad - len;
        for (size_t k = 0; k < add; k++) ub_put(&pr->out, ' ');
        memmove(pr->out.u + start + add, pr->out.u + start, len * sizeof(uint16_t));
        for (size_t k = 0; k < add; k++) pr->out.u[start + k] = ' ';
    }
}

dp_result dp_format(const uint16_t *pat, size_t np, int64_t secs, int32_t nanos,
                    uint16_t **out, size_t *nout, const char **msg)
{
    elem_list *l = compile(pat, np, false, msg);
    if (l == NULL) return DP_THROWN;
    int64_t day = date_floordiv(secs, 86400);
    if (day < EPOCH_DAY_MIN || day > EPOCH_DAY_MAX) {
        *msg = msgf("java.time.DateTimeException: Invalid value for EpochDay (valid values "
                    "-365243219162 - 365241780471): %lld", (long long)day);
        return DP_THROWN;
    }
    int64_t nod = (secs - day * 86400) * NANOS_PER_SECOND + nanos;
    int64_t fv[F_COUNT];
    fields_of(day, nod, fv);
    printer pr;
    memset(&pr, 0, sizeof(pr));
    pr.fv = fv;
    pr.minute = (int)(nod / (60 * NANOS_PER_SECOND));
    print_list(&pr, l->v, l->n);
    if (pr.err != NULL) {
        *msg = pr.err;
        return DP_THROWN;
    }
    *out = pr.out.u;
    *nout = pr.out.n;
    return DP_OK;
}

/* --- parsing ------------------------------------------------------ */

typedef struct {
    bool has[F_COUNT];
    int64_t v[F_COUNT];
    uint32_t seq[F_COUNT];    /* when each field was set, for messages */
    uint32_t next_seq;
    int period;               /* day period, or -1 */
    bool has_zone;
    int32_t zone;             /* a fixed zone's offset in seconds */
    int zone_region;          /* a region's index, or -1 for a fixed zone */
    const char *zone_text;    /* the zone as Java prints it */
    int zone_type;            /* the kind of zone name read, or -1 */
} fields;

typedef struct {
    const uint16_t *t;
    size_t len;               /* end of the text, or of a pad's region */
    fields f;
    const char *abort;        /* an exception that ends the parse */
} parser;

static bool is_digit(uint16_t c) { return c >= '0' && c <= '9'; }

static bool starts_with(const parser *ps, long pos, const char *s)
{
    size_t n = strlen(s);
    if ((size_t)pos + n > ps->len) return false;
    for (size_t i = 0; i < n; i++) if (ps->t[pos + i] != (uint8_t)s[i]) return false;
    return true;
}

static void put_field(fields *f, int field, int64_t v)
{
    if (!f->has[field]) f->seq[field] = f->next_seq++;
    f->has[field] = true;
    f->v[field] = v;
}

/* Sets a parsed field. A different value for a field already parsed
 * fails the element at `errpos`. */
static long set_parsed(parser *ps, int field, int64_t v, long errpos, long next)
{
    if (ps->f.has[field] && ps->f.v[field] != v) return ~errpos;
    put_field(&ps->f, field, v);
    return next;
}

static long parse_number(parser *ps, const elem *e, long pos)
{
    long p = pos;
    bool neg = false, plus = false;
    if ((size_t)p < ps->len) {
        uint16_t ch = ps->t[p];
        if (ch == '+') {
            if (e->sign != SS_EXCEEDS) return ~pos;
            plus = true;
            p++;
        } else if (ch == '-') {
            if (e->sign == SS_NOT_NEG) return ~pos;
            neg = true;
            p++;
        }
    }
    long limit = e->maxw + (e->reserve > 0 ? e->reserve : 0);
    long cnt = 0;
    while ((size_t)(p + cnt) < ps->len && cnt < limit && is_digit(ps->t[p + cnt])) cnt++;
    if (cnt < e->minw) return ~p;
    long take = cnt;
    if (e->reserve > 0) {
        take = cnt - e->reserve;
        if (take < e->minw) take = e->minw;
    }
    if (take > e->maxw) take = e->maxw;

    /* Nineteen digits that do not fit in a long leave the last one. */
    uint64_t mag = 0;
    for (long i = 0; i < take; i++) {
        uint64_t d = (uint64_t)(ps->t[p + i] - '0');
        if (i == 18) {
            uint64_t lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
            if (mag > (lim - d) / 10) { take = 18; break; }
        }
        mag = mag * 10 + d;
    }

    if (neg && mag == 0) return ~pos;
    if (e->sign == SS_EXCEEDS) {
        if (!plus && !neg && take > e->minw) return ~pos;
        if (plus && take <= e->minw) return ~pos;
    }
    int64_t v;
    if (e->kind == EK_FRAC) {
        v = (int64_t)mag;
        for (int i = e->minw; i < 9; i++) v *= 10;
    } else if (e->kind == EK_REDUCED) {
        v = 2000 + (int64_t)mag;
    } else {
        v = neg ? (int64_t)(0 - mag) : (int64_t)mag;
    }
    return set_parsed(ps, e->field, v, p, p + take);
}

/* Matches the longest name in a set; of equal names, the last wins. */
static long match_name(const parser *ps, long pos, const char *const *names, int count, int *which)
{
    long best = -1;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(names[i]);
        if ((long)n >= best && starts_with(ps, pos, names[i])) {
            best = (long)n;
            *which = i;
        }
    }
    return best;
}

static long parse_text(parser *ps, const elem *e, long pos)
{
    name_set s = names_for(e->field, e->style);
    int which = 0;
    long n = match_name(ps, pos, s.names, s.count, &which);
    if (n < 0) return ~pos;
    return set_parsed(ps, e->field, which + s.first, pos, pos + n);
}

static long parse_period(parser *ps, const elem *e, long pos)
{
    int which = 0;
    long n = match_name(ps, pos, PERIOD_NAMES[e->style], DP_PERIODS, &which);
    if (n < 0) return ~pos;
    ps->f.period = which;   /* a later period replaces an earlier one */
    return pos + n;
}

/* Reads exactly two digits at *p. */
static bool two_digits(const parser *ps, long *p, int *out)
{
    if ((size_t)*p + 2 > ps->len || !is_digit(ps->t[*p]) || !is_digit(ps->t[*p + 1])) return false;
    *out = (ps->t[*p] - '0') * 10 + (ps->t[*p + 1] - '0');
    *p += 2;
    return true;
}

/* One part of an X-style offset: two digits, 59 at most. */
static bool offset_part(const parser *ps, long *p, int *out)
{
    long q = *p;
    if (!two_digits(ps, &q, out) || *out > 59) return false;
    *p = q;
    return true;
}

/* An offset in one of the X layouts after its sign. Returns the end, or
 * -1 when the text does not fit the layout. */
static long offset_body(parser *ps, int layout, long p, int *h, int *m, int *s)
{
    *h = *m = *s = 0;
    if (!offset_part(ps, &p, h)) return -1;
    bool colon = layout == OF_HH_MM || layout == OF_HH_MM_ss;
    bool need_min = layout != OF_HHmm;
    long q = p;
    if (colon) {
        if ((size_t)q < ps->len && ps->t[q] == ':') q++;
        else if (need_min) return -1;
        else return p;
    }
    if (!offset_part(ps, &q, m)) {
        if (need_min) return -1;
        return p;
    }
    p = q;
    if (layout == OF_HHMMss || layout == OF_HH_MM_ss) {
        q = p;
        if (colon) {
            if ((size_t)q < ps->len && ps->t[q] == ':') q++;
            else return p;
        }
        if (offset_part(ps, &q, s)) p = q;
    }
    return p;
}

/* An hour from 24 to 59 throws. */
static long offset_value(parser *ps, int sign, int h, int m, int s, long pos, long *total)
{
    if (h > 23) {
        ps->abort = "Value out of range: Hour[0-23], Minute[0-59], Second[0-59]";
        return ~pos;
    }
    *total = sign * (h * 3600L + m * 60L + s);
    return 0;
}

static long parse_offset(parser *ps, const elem *e, long pos)
{
    if (starts_with(ps, pos, e->notext)) {
        return set_parsed(ps, F_OFFSET, 0, pos, pos + (long)strlen(e->notext));
    }
    if ((size_t)pos >= ps->len || (ps->t[pos] != '+' && ps->t[pos] != '-')) return ~pos;
    int sign = ps->t[pos] == '-' ? -1 : 1;
    int h, m, s;
    long end = offset_body(ps, e->style, pos + 1, &h, &m, &s);
    if (end < 0) return ~pos;
    long total = 0;
    long r = offset_value(ps, sign, h, m, s, pos, &total);
    if (r < 0) return r;
    return set_parsed(ps, F_OFFSET, total, pos, end);
}

/* "GMT" with an optional offset: O reads +H, +HH, +H:mm and +H:mm:ss;
 * OOOO and ZZZZ read +HH:mm and +HH:mm:ss. */
static long parse_local_offset(parser *ps, const elem *e, long pos)
{
    if (!starts_with(ps, pos, "GMT")) return ~pos;
    long p = pos + 3;
    if ((size_t)p >= ps->len || (ps->t[p] != '+' && ps->t[p] != '-')) {
        return set_parsed(ps, F_OFFSET, 0, pos, p);
    }
    int sign = ps->t[p] == '-' ? -1 : 1;
    p++;
    int h = 0, m = 0, s = 0;
    if (e->style == ST_SHORT) {
        /* Java reads the first digit without checking the length. */
        if ((size_t)p >= ps->len) {
            ps->abort = msgf("Index %ld out of bounds for length %zu", p, ps->len);
            return ~pos;
        }
        if (!is_digit(ps->t[p])) return ~pos;
        h = ps->t[p++] - '0';
        if ((size_t)p < ps->len && is_digit(ps->t[p])) h = h * 10 + (ps->t[p++] - '0');
        long q = p;
        if ((size_t)q < ps->len && ps->t[q] == ':' && (q++, two_digits(ps, &q, &m))) {
            p = q;
            if ((size_t)q < ps->len && ps->t[q] == ':' && (q++, two_digits(ps, &q, &s))) p = q;
        }
    } else {
        /* Java reads HH:MM without checking the length first. */
        uint16_t ch[5];
        for (int i = 0; i < 5; i++) {
            if ((size_t)(p + i) >= ps->len) {
                ps->abort = msgf("Index %ld out of bounds for length %zu", p + i, ps->len);
                return ~pos;
            }
            ch[i] = ps->t[p + i];
            if (i == 1 && (!is_digit(ch[0]) || !is_digit(ch[1]))) return ~pos;
            if (i == 2 && ch[2] != ':') return ~pos;
        }
        if (!is_digit(ch[3]) || !is_digit(ch[4])) return ~pos;
        h = (ch[0] - '0') * 10 + (ch[1] - '0');
        m = (ch[3] - '0') * 10 + (ch[4] - '0');
        p += 5;
        long q = p;
        if ((size_t)q < ps->len && ps->t[q] == ':' && (q++, two_digits(ps, &q, &s))) p = q;
    }
    /* The range is checked only when the offset is used. */
    return set_parsed(ps, F_OFFSET, sign * (h * 3600L + m * 60L + s), pos, p);
}

/* A ZoneOffset as Java prints it: Z, +05:00 or -05:30:15. */
static const char *offset_text(long total)
{
    if (total == 0) return "Z";
    long a = total < 0 ? -total : total;
    char sign = total < 0 ? '-' : '+';
    if (a % 60 != 0) return msgf("%c%02ld:%02ld:%02ld", sign, a / 3600, a / 60 % 60, a % 60);
    return msgf("%c%02ld:%02ld", sign, a / 3600, a / 60 % 60);
}

/* A zone parsed from the text. */
typedef struct {
    int32_t offset;           /* a fixed zone's offset */
    int region;               /* a region's index, or -1 */
    const char *text;
    int type;                 /* the kind of name, or -1 for an ID */
    bool named;               /* read by name or region ID, which sets type */
} zone_read;

/* A zone ID: an offset, UTC, GMT or UT with an optional offset after
 * it, or a region ID or zone name as tz_match reads one. `names` is 0
 * for no names, 1 for the short ones (z, v) and 2 for the long ones
 * (zzzz, vvvv). A lone Z is the zero offset. Returns the end, or ~pos. */
static long parse_zone_id(parser *ps, long pos, int names, zone_read *z)
{
    z->offset = 0;
    z->region = -1;
    z->text = "";
    z->type = -1;
    z->named = false;
    if ((size_t)pos >= ps->len) return ~pos;
    uint16_t ch = ps->t[pos];
    if (ch == '+' || ch == '-') {
        int h, m, s;
        long end = offset_body(ps, OF_HH_MM_ss, pos + 1, &h, &m, &s);
        if (end < 0) return ~pos;
        long total = 0;
        if (offset_value(ps, ch == '-' ? -1 : 1, h, m, s, pos, &total) < 0) return ~pos;
        if (total < -64800 || total > 64800) return ~pos;
        z->offset = (int32_t)total;
        z->text = offset_text(total);
        return end;
    }
    long p;
    const char *prefix;
    if (starts_with(ps, pos, "GMT0")) {
        z->region = tz_region_named("GMT0");
        z->text = "GMT0";
        return pos + 4;
    }
    if (starts_with(ps, pos, "UTC")) { p = pos + 3; prefix = "UTC"; }
    else if (starts_with(ps, pos, "GMT")) { p = pos + 3; prefix = "GMT"; }
    else if (starts_with(ps, pos, "UT")) { p = pos + 2; prefix = "UT"; }
    else {
        size_t rlen = 0;
        int type = -1;
        int region = tz_match(names, ps->t + pos, ps->len - (size_t)pos, &rlen, &type);
        if (region >= 0) {
            z->region = region;
            z->text = tz_region_id(region);
            z->type = type;
            z->named = true;
            return pos + (long)rlen;
        }
        if (ch == 'Z') {
            z->text = "Z";
            return pos + 1;
        }
        return ~pos;
    }
    z->text = prefix;
    if ((size_t)p < ps->len && (ps->t[p] == '+' || ps->t[p] == '-')) {
        int h, m, s;
        long end = offset_body(ps, OF_HH_MM_ss, p + 1, &h, &m, &s);
        long total = 0;
        if (end >= 0) {
            /* A well-formed offset out of range fails the whole ID. */
            if (offset_value(ps, ps->t[p] == '-' ? -1 : 1, h, m, s, p, &total) < 0 ||
                total < -64800 || total > 64800) return ~pos;
            z->offset = (int32_t)total;
            if (total != 0) z->text = msgf("%s%s", prefix, offset_text(total));
            return end;
        }
    }
    return p;
}

static long parse_zone(parser *ps, const elem *e, long pos)
{
    zone_read z;
    long end = parse_zone_id(ps, pos, e->kind != EK_ZONETEXT ? 0 : e->style == 4 ? 2 : 1, &z);
    if (end < 0) return end;
    ps->f.has_zone = true;
    ps->f.zone = z.offset;
    ps->f.zone_region = z.region;
    ps->f.zone_text = z.text;
    if (z.named) ps->f.zone_type = z.type;
    return end;
}

static long parse_list(parser *ps, const elem *v, size_t n, long pos);

static long parse_core(parser *ps, const elem *e, long pos)
{
    switch (e->kind) {
    case EK_LIT:
        if ((size_t)pos + e->nlit > ps->len) return ~pos;
        for (size_t i = 0; i < e->nlit; i++) if (ps->t[pos + i] != e->lit[i]) return ~pos;
        return pos + (long)e->nlit;
    case EK_NUM: case EK_REDUCED: case EK_FRAC:
        return parse_number(ps, e, pos);
    case EK_TEXT:
        return parse_text(ps, e, pos);
    case EK_PERIOD:
        return parse_period(ps, e, pos);
    case EK_OFFSET:
        return parse_offset(ps, e, pos);
    case EK_LOCOFF:
        return parse_local_offset(ps, e, pos);
    case EK_ZONEID: case EK_ZONETEXT:
        return parse_zone(ps, e, pos);
    case EK_OPT: {
        fields saved = ps->f;
        long r = parse_list(ps, e->kids, e->nkids, pos);
        if (r < 0 && ps->abort == NULL) {
            ps->f = saved;
            return pos;
        }
        return r;
    }
    case EK_DEFAULTS:
        /* parseDefaulting: hour, minute and second are 0 unless parsed. */
        if (!ps->f.has[F_HOUR]) put_field(&ps->f, F_HOUR, 0);
        if (!ps->f.has[F_MIN]) put_field(&ps->f, F_MIN, 0);
        if (!ps->f.has[F_SEC]) put_field(&ps->f, F_SEC, 0);
        return pos;
    }
    return ~pos;
}

static long parse_elem(parser *ps, const elem *e, long pos)
{
    if (e->pad == 0) return parse_core(ps, e, pos);
    long end = pos + e->pad;
    if ((size_t)end > ps->len) return ~pos;
    long p = pos;
    while (p < end && ps->t[p] == ' ') p++;
    size_t saved = ps->len;
    ps->len = (size_t)end;
    long r = parse_core(ps, e, p);
    ps->len = saved;
    if (r != end) return ~(pos + p);
    return end;
}

static long parse_list(parser *ps, const elem *v, size_t n, long pos)
{
    for (size_t i = 0; i < n; i++) {
        pos = parse_elem(ps, &v[i], pos);
        if (pos < 0 || ps->abort != NULL) return pos < 0 ? pos : ~pos;
    }
    return pos;
}

/* --- resolving ---------------------------------------------------- */

typedef struct {
    fields *f;
    const char *err;
    bool has_date;
    int64_t day;              /* epoch day */
    int64_t nod;              /* nano of day */
    bool has_time;
    bool excess_day;          /* 24:00 */
    bool period_used;         /* the day period picked the hour */
    bool check_period;        /* whether the day period is checked */
} resolver;

static bool check(resolver *r, int field, int64_t v)
{
    const field_info *fi = &FIELDS[field];
    if (v < fi->lo || v > fi->hi) {
        r->err = msgf("Invalid value for %s (valid values %s): %lld", fi->name, fi->range, (long long)v);
        return false;
    }
    return true;
}

static int64_t take(fields *f, int field)
{
    f->has[field] = false;
    return f->v[field];
}

static void set_date(resolver *r, int64_t day)
{
    if (r->err != NULL) return;
    if (day < EPOCH_DAY_MIN || day > EPOCH_DAY_MAX) {
        r->err = msgf("Invalid value for EpochDay (valid values -365243219162 - 365241780471): %lld",
                      (long long)day);
        return;
    }
    if (r->has_date && r->day != day) {
        r->err = msgf("Conflict found: Fields resolved to two different dates: %s %s",
                      date_text(r->day), date_text(day));
        return;
    }
    r->has_date = true;
    r->day = day;
}

/* Year of era and era into the proleptic year. */
static void resolve_year(resolver *r)
{
    fields *f = r->f;
    if (!f->has[F_YOE]) return;
    int64_t yoe = take(f, F_YOE);
    if (!check(r, F_YOE, yoe)) return;
    int64_t year;
    if (f->has[F_ERA]) {
        int64_t era = take(f, F_ERA);
        if (!check(r, F_ERA, era)) return;
        year = era == 1 ? yoe : 1 - yoe;
    } else if (f->has[F_YEAR]) {
        year = f->v[F_YEAR] > 0 ? yoe : 1 - yoe;
    } else {
        year = yoe;
    }
    if (f->has[F_YEAR] && f->v[F_YEAR] != year) {
        r->err = msgf("Conflict found: Year %lld differs from Year %lld",
                      (long long)f->v[F_YEAR], (long long)year);
        return;
    }
    put_field(f, F_YEAR, year);
}

/* The localized day of week becomes the ISO one, replacing any parsed. */
static void resolve_local_dow(resolver *r)
{
    fields *f = r->f;
    if (f->has[F_LDOW]) {
        int64_t ld = take(f, F_LDOW);
        if (!check(r, F_LDOW, ld)) return;
        put_field(f, F_DOW, (ld + 5) % 7 + 1);
    }
}

/* IsoChronology.resolveDate in SMART mode. */
static void resolve_iso_date(resolver *r)
{
    fields *f = r->f;
    if (!f->has[F_YEAR]) return;
    if (f->has[F_MONTH] && f->has[F_DOM]) {
        int64_t y = f->v[F_YEAR], m = f->v[F_MONTH], d = f->v[F_DOM];
        if (!check(r, F_YEAR, y) || !check(r, F_MONTH, m) || !check(r, F_DOM, d)) return;
        take(f, F_YEAR); take(f, F_MONTH); take(f, F_DOM);
        int len = date_days_in_month(y, (int)m);
        if (d > len) d = len;
        set_date(r, date_days_from_civil(y, (int)m, (int)d));
        return;
    }
    if (f->has[F_MONTH] && f->has[F_AWOM] && f->has[F_DOW]) {
        int64_t y = f->v[F_YEAR], m = f->v[F_MONTH], w = f->v[F_AWOM], dow = f->v[F_DOW];
        if (!check(r, F_YEAR, y) || !check(r, F_MONTH, m) || !check(r, F_AWOM, w) ||
            !check(r, F_DOW, dow)) return;
        take(f, F_YEAR); take(f, F_MONTH); take(f, F_AWOM); take(f, F_DOW);
        int64_t day = date_days_from_civil(y, (int)m, 1) + (w - 1) * 7;
        day += date_floormod(dow - iso_dow(day), 7);
        set_date(r, day);
        return;
    }
    if (f->has[F_DOY]) {
        int64_t y = f->v[F_YEAR], doy = f->v[F_DOY];
        if (!check(r, F_YEAR, y) || !check(r, F_DOY, doy)) return;
        take(f, F_YEAR); take(f, F_DOY);
        if (doy == 366 && !date_is_leap(y)) {
            r->err = msgf("Invalid date 'DayOfYear 366' as '%lld' is not a leap year", (long long)y);
            return;
        }
        set_date(r, date_days_from_civil(y, 1, 1) + doy - 1);
    }
}

/* The week fields of WeekFields(SUNDAY, 1), and the modified Julian
 * day. */
static void resolve_other_dates(resolver *r)
{
    fields *f = r->f;
    if (f->has[F_DOW]) {
        if (f->has[F_YEAR]) {
            if (f->has[F_MONTH] && f->has[F_WOM]) {
                int64_t y = f->v[F_YEAR], m = f->v[F_MONTH], w = f->v[F_WOM], dow = f->v[F_DOW];
                if (!check(r, F_YEAR, y) || !check(r, F_MONTH, m) || !check(r, F_WOM, w) ||
                    !check(r, F_DOW, dow)) return;
                take(f, F_YEAR); take(f, F_MONTH); take(f, F_WOM); take(f, F_DOW);
                int64_t first = date_days_from_civil(y, (int)m, 1);
                set_date(r, first + (w - 1) * 7 + (local_dow((int)dow) - local_dow(iso_dow(first))));
                if (r->err != NULL) return;
            }
        } else if (f->has[F_WBY] && f->has[F_WOWBY]) {
            int64_t y = f->v[F_WBY], w = f->v[F_WOWBY], dow = f->v[F_DOW];
            if (!check(r, F_WBY, y) || !check(r, F_DOW, dow)) return;
            int64_t start = wby_start(y);
            int64_t weeks = y < 999999999LL ? (wby_start(y + 1) - start) / 7 : 52;
            if (w < 1 || w > weeks) {
                r->err = msgf("Invalid value for %s (valid values 1 - %lld): %lld",
                              FIELDS[F_WOWBY].name, (long long)weeks, (long long)w);
                return;
            }
            take(f, F_WBY); take(f, F_WOWBY); take(f, F_DOW);
            set_date(r, start + (w - 1) * 7 + (local_dow((int)dow) - 1));
            if (r->err != NULL) return;
        }
    }
    if (f->has[F_MJD]) {
        int64_t mjd = take(f, F_MJD);
        if (!check(r, F_MJD, mjd)) return;
        set_date(r, mjd - MJD_OFFSET);
    }
}

/* Moves a value into a time field, failing on a different value. */
static bool put_time(resolver *r, int field, int64_t v, int from)
{
    fields *f = r->f;
    if (f->has[field] && f->v[field] != v) {
        r->err = msgf("Conflict found: %s %lld differs from %s %lld while resolving  %s",
                      FIELDS[field].name, (long long)f->v[field], FIELDS[field].name,
                      (long long)v, FIELDS[from].name);
        return false;
    }
    put_field(f, field, v);
    return true;
}

static void resolve_time(resolver *r)
{
    fields *f = r->f;
    if (f->has[F_CHOD]) {
        int64_t v = take(f, F_CHOD);
        if (v != 0 && !check(r, F_CHOD, v)) return;
        if (!put_time(r, F_HOUR, v == 24 ? 0 : v, F_CHOD)) return;
    }
    if (f->has[F_CHOA]) {
        int64_t v = take(f, F_CHOA);
        if (v != 0 && !check(r, F_CHOA, v)) return;
        if (!put_time(r, F_HOA, v == 12 ? 0 : v, F_CHOA)) return;
    }
    if (f->has[F_AMPM] && !check(r, F_AMPM, f->v[F_AMPM])) return;
    if (f->has[F_HOA] && !check(r, F_HOA, f->v[F_HOA])) return;
    if (f->has[F_AMPM] && f->has[F_HOA]) {
        int64_t ap = take(f, F_AMPM), h = take(f, F_HOA);
        if (!put_time(r, F_HOUR, ap * 12 + h, F_AMPM)) return;
    }
    if (f->has[F_NOD]) {
        int64_t v = take(f, F_NOD);
        if (!check(r, F_NOD, v)) return;
        if (!put_time(r, F_HOUR, v / 3600000000000LL, F_NOD) ||
            !put_time(r, F_MIN, v / 60000000000LL % 60, F_NOD) ||
            !put_time(r, F_SEC, v / NANOS_PER_SECOND % 60, F_NOD) ||
            !put_time(r, F_NANO, v % NANOS_PER_SECOND, F_NOD)) return;
    }
    if (f->has[F_MOD]) {
        int64_t v = take(f, F_MOD);
        if (!check(r, F_MOD, v)) return;
        if (!put_time(r, F_SOD, v / 1000, F_MOD) || !put_time(r, F_MILLI, v % 1000, F_MOD)) return;
    }
    if (f->has[F_SOD]) {
        int64_t v = take(f, F_SOD);
        if (!put_time(r, F_HOUR, v / 3600, F_SOD) || !put_time(r, F_MIN, v / 60 % 60, F_SOD) ||
            !put_time(r, F_SEC, v % 60, F_SOD)) return;
    }
    if (f->has[F_MILLI]) {
        int64_t v = take(f, F_MILLI);
        int64_t below = f->has[F_NANO] ? f->v[F_NANO] % 1000000 : 0;
        if (!put_time(r, F_NANO, v * 1000000 + below, F_MILLI)) return;
    }

    /* A day period with an hour of am/pm picks the hour in the
     * afternoon when that one falls in the period. */
    if (f->period >= 0 && f->has[F_HOA] && !f->has[F_AMPM]) {
        int64_t h = take(f, F_HOA);
        if (!period_has_minute(f->period, (int)h * 60) &&
            period_has_minute(f->period, (int)(h + 12) * 60)) h += 12;
        if (!put_time(r, F_HOUR, h, F_HOA)) return;
        r->period_used = true;
    }

    /* Java checks a day period against the time only when the period
     * did not pick the hour and no fraction of a second was parsed. */
    r->check_period = f->period >= 0 && !r->period_used && !f->has[F_NANO];
    if (!f->has[F_HOUR]) {
        /* With no clock fields at all, a day period, or else am/pm,
         * gives the middle of its span. */
        if (f->has[F_MIN] || f->has[F_SEC] || f->has[F_NANO] || f->has[F_HOA]) return;
        int p = f->period >= 0 ? f->period : f->has[F_AMPM] ? DP_AM + (int)f->v[F_AMPM] : -1;
        if (p < 0) return;
        int from = PERIOD_FROM[p], to = PERIOD_TO[p];
        if (to < from) to += 1440;
        r->nod = (int64_t)((from + to) / 2 % 1440) * 60 * NANOS_PER_SECOND;
        r->has_time = true;
        r->check_period = false;
        return;
    }
    /* Seconds need minutes, and a fraction needs seconds. */
    if ((!f->has[F_MIN] && (f->has[F_SEC] || f->has[F_NANO])) ||
        (!f->has[F_SEC] && f->has[F_NANO])) return;
    int64_t h = f->v[F_HOUR];
    int64_t mi = f->has[F_MIN] ? f->v[F_MIN] : 0;
    int64_t s = f->has[F_SEC] ? f->v[F_SEC] : 0;
    int64_t n = f->has[F_NANO] ? f->v[F_NANO] : 0;
    if (h == 24 && mi == 0 && s == 0 && n == 0) {
        r->excess_day = true;
        h = 0;
    } else if (!check(r, F_HOUR, h)) {
        return;
    }
    if (!check(r, F_MIN, mi) || !check(r, F_SEC, s) || !check(r, F_NANO, n)) return;
    take(f, F_HOUR);
    if (f->has[F_MIN]) take(f, F_MIN);
    if (f->has[F_SEC]) take(f, F_SEC);
    if (f->has[F_NANO]) take(f, F_NANO);
    r->has_time = true;
    r->nod = ((h * 60 + mi) * 60 + s) * NANOS_PER_SECOND + n;
}

/* Fields left over must agree with the date (time = false) or the time
 * (time = true) they resolved to. */
static void cross_check(resolver *r, bool time)
{
    fields *f = r->f;
    int64_t fv[F_COUNT];
    fields_of(r->has_date ? r->day : 0, r->nod, fv);
    static const int date_fields[] = {
        F_ERA, F_YEAR, F_MONTH, F_DOM, F_DOY, F_DOW, F_AWOM, F_QOY, F_WBY, F_WOWBY, F_WOM
    };
    static const int time_fields[] = { F_AMPM, F_HOA };

    for (;;) {
        /* In the order the fields were parsed. */
        int pick = -1;
        uint32_t best = UINT32_MAX;
        for (size_t i = 0; !time && i < sizeof(date_fields) / sizeof(date_fields[0]); i++) {
            int fd = date_fields[i];
            if (r->has_date && f->has[fd] && f->seq[fd] < best) { best = f->seq[fd]; pick = fd; }
        }
        for (size_t i = 0; time && i < sizeof(time_fields) / sizeof(time_fields[0]); i++) {
            int fd = time_fields[i];
            if (r->has_time && f->has[fd] && f->seq[fd] < best) { best = f->seq[fd]; pick = fd; }
        }
        if (pick < 0) return;
        bool is_time = pick == F_AMPM || pick == F_HOA;
        if (fv[pick] != f->v[pick]) {
            r->err = msgf("Conflict found: Field %s %lld differs from %s %lld derived from %s",
                          FIELDS[pick].name, (long long)fv[pick], FIELDS[pick].name,
                          (long long)f->v[pick],
                          is_time ? time_text(r->nod) : date_text(r->day));
            return;
        }
        take(f, pick);
    }
}

/* What is left of the fields, as Java's Parsed prints them. */
static char *fields_text(fields *f)
{
    char *s = msgf("%s", "");
    bool first = true;
    for (;;) {
        int pick = -1;
        uint32_t best = UINT32_MAX;
        for (int i = 0; i < F_COUNT; i++) {
            if (f->has[i] && f->seq[i] < best) { best = f->seq[i]; pick = i; }
        }
        if (pick < 0) break;
        s = msgf("%s%s%s=%lld", s, first ? "" : ", ", FIELDS[pick].name, (long long)f->v[pick]);
        first = false;
        f->has[pick] = false;
    }
    return s;
}

static char *text_echo(const uint16_t *t, size_t n)
{
    size_t nb;
    if (n > 64) return msgf("%s...", av_units_to_text(t, 64, &nb));
    return av_units_to_text(t, n, &nb);
}

/* Returned by parse_with on the first pass when the text has no hour,
 * so the parse is run again with the defaults. */
#define DP_NO_HOUR ((dp_result)-1)

/* Parses and resolves the text with the compiled pattern `l`, then
 * reads the instant from it. On the first pass (`first`, a pattern
 * without the defaults) the parse stops with DP_NO_HOUR once the
 * fields are resolved if neither the hour of day nor the hour of am/pm
 * is among them. */
static dp_result parse_with(const uint16_t *text, size_t nt, elem_list *l, bool first,
                            int64_t *secs, int32_t *nanos, const char **msg)
{
    parser ps;
    memset(&ps, 0, sizeof(ps));
    ps.t = text;
    ps.len = nt;
    ps.f.period = -1;
    ps.f.zone_region = -1;
    ps.f.zone_type = -1;
    long pos = parse_list(&ps, l->v, l->n, 0);
    char *echo = text_echo(text, nt);
    if (ps.abort != NULL) {
        *msg = msgf("Text '%s' could not be parsed: %s", echo, ps.abort);
        return DP_PARSE_ERROR;
    }
    if (pos < 0) {
        *msg = msgf("Text '%s' could not be parsed at index %ld", echo, ~pos);
        return DP_PARSE_ERROR;
    }
    if ((size_t)pos != nt) {
        *msg = msgf("Text '%s' could not be parsed, unparsed text found at index %ld", echo, pos);
        return DP_PARSE_ERROR;
    }

    fields *f = &ps.f;

    resolver r;
    memset(&r, 0, sizeof(r));
    r.f = f;
    resolve_year(&r);
    if (r.err == NULL) resolve_local_dow(&r);
    if (r.err == NULL) resolve_iso_date(&r);
    if (r.err == NULL) resolve_other_dates(&r);
    if (r.err == NULL) resolve_time(&r);
    if (r.err == NULL) cross_check(&r, false);
    if (r.err == NULL && r.check_period && r.has_time) {
        int minute = (int)(r.nod / (60 * NANOS_PER_SECOND));
        if (!period_has_minute(f->period, minute)) {
            int p = f->period;
            char *span = PERIOD_FROM[p] == PERIOD_TO[p]
                ? msgf("%02d:%02d", PERIOD_FROM[p] / 60, PERIOD_FROM[p] % 60)
                : msgf("%02d:%02d-%02d:%02d", PERIOD_FROM[p] / 60, PERIOD_FROM[p] % 60,
                       PERIOD_TO[p] / 60, PERIOD_TO[p] % 60);
            r.err = msgf("Conflict found: Resolved time %s conflicts with DayPeriod(%s)",
                         time_text(r.nod / (60 * NANOS_PER_SECOND) * 60 * NANOS_PER_SECOND), span);
        }
    }
    if (r.err == NULL) cross_check(&r, true);
    if (r.err == NULL && r.excess_day && r.has_date) {
        if (r.day + 1 > EPOCH_DAY_MAX) {
            r.err = msgf("Invalid value for EpochDay (valid values -365243219162 - 365241780471): %lld",
                         (long long)(r.day + 1));
        } else {
            r.day++;
        }
    }
    /* Parsed.isSupported(HOUR_OF_DAY) or (HOUR_OF_AMPM): a resolved
     * time, or either field left over. */
    if (first && r.err == NULL && !r.has_time && !f->has[F_HOUR] && !f->has[F_HOA]) {
        return DP_NO_HOUR;
    }
    if (r.err == NULL && (!r.has_date || !r.has_time)) {
        const char *got = r.has_date ? msgf(" resolved to %s", date_text(r.day))
                  : r.has_time ? msgf(" resolved to %s", time_text(r.nod)) : "";
        r.err = msgf("Unable to obtain ZonedDateTime from TemporalAccessor: {%s},ISO,%s,%d%s "
                     "of type java.time.format.Parsed", fields_text(f),
                     f->has_zone ? f->zone_text : "Z", f->zone_type, got);
    }
    int64_t offset = 0;
    if (r.err == NULL) {
        if (f->has[F_OFFSET]) {
            offset = f->v[F_OFFSET];
        } else if (f->has_zone && f->zone_region >= 0) {
            /* In a gap the time moves on by the gap's length; in an
             * overlap a standard or generic zone name picks the later
             * offset, and anything else the earlier one. */
            int32_t earlier, later;
            int64_t local = r.day * 86400 + r.nod / NANOS_PER_SECOND;
            tz_local_kind k = tz_local_offsets(f->zone_region, local, &earlier, &later);
            if (k == TZ_OVERLAP && (f->zone_type == 0 || f->zone_type == 2)) offset = later;
            else offset = earlier;
        } else if (f->has_zone) {
            offset = f->zone;
        }
        if (offset < -64800 || offset > 64800) {
            r.err = "Zone offset not in valid range: -18:00 to +18:00";
        }
    }
    if (r.err != NULL) {
        *msg = msgf("Text '%s' could not be parsed: %s", echo, r.err);
        return DP_PARSE_ERROR;
    }
    *secs = r.day * 86400 + r.nod / NANOS_PER_SECOND - offset;
    *nanos = (int32_t)(r.nod % NANOS_PER_SECOND);
    return DP_OK;
}

dp_result dp_parse(const uint16_t *text, size_t nt, const uint16_t *pat, size_t np,
                   int64_t *secs, int32_t *nanos, const char **msg)
{
    elem_list *l = compile(pat, np, false, msg);
    if (l == NULL) return DP_THROWN;
    dp_result r = parse_with(text, nt, l, true, secs, nanos, msg);
    if (r != DP_NO_HOUR) return r;
    l = compile(pat, np, true, msg);
    if (l == NULL) return DP_THROWN;
    return parse_with(text, nt, l, false, secs, nanos, msg);
}
