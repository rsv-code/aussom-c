/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Named time zones, as java.time's tzdb provider offers them.
 *
 * Building blocks:
 *
 * - The data is the IANA time zone database in zic's input format,
 *   shrunk into one file (third_party/tzdata/tzdata.zi) and embedded as
 *   the string aussom_tzdata_zi. Three kinds of line matter: a Rule (R)
 *   says when a daylight saving change happens and how much it saves; a
 *   Zone (Z) is a list of lines, each giving a standard offset, the
 *   rules or fixed saving in force, and the local time it lasts until;
 *   a Link (L) is a second name for a zone.
 * - The file is read the first time a named zone is needed. Each zone's
 *   transitions (the instants its offset changes) are worked out the
 *   first time that zone is used, for the years up to 2100. Later years
 *   repeat the zone's last rules, which is what java.time does too.
 * - Region IDs are the zone and link names, less the ones java.time
 *   leaves out (EST, HST, MST, ROC, GMT+0 and GMT-0) and plus the
 *   SystemV zones it keeps from the old IANA systemv file.
 * - Zone names (PST, "Pacific Standard Time") are CLDR's en_US names as
 *   the JDK 23 runtime parses them, each with the region it chose and
 *   the kind of name.
 *
 * The rules were checked against the JDK runtime's results for every
 * region. No JDK source was used.
 */

#include "tzdb.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av_string.h"
#include "date_pattern.h"

/* tzdata.zi, embedded by the Makefile. */
extern const char aussom_tzdata_zi[];

/* The years worked out in advance. Later years repeat the last rules. */
#define LAST_YEAR 2100

/* java.time's own additions, from the old IANA systemv file. The file
 * starts its rules at "min"; java.time starts them at 1900. */
static const char SYSTEMV_ZI[] =
    "R SystemV 1900 1973 - Ap lastSu 2 1 D\n"
    "R SystemV 1900 1973 - O lastSu 2 0 S\n"
    "R SystemV 1974 o - Ja 6 2 1 D\n"
    "R SystemV 1974 o - N lastSu 2 0 S\n"
    "R SystemV 1975 o - F 23 2 1 D\n"
    "R SystemV 1975 o - O lastSu 2 0 S\n"
    "R SystemV 1976 ma - Ap lastSu 2 1 D\n"
    "R SystemV 1976 ma - O lastSu 2 0 S\n"
    "Z SystemV/AST4ADT -4 SystemV A%sT\n"
    "Z SystemV/EST5EDT -5 SystemV E%sT\n"
    "Z SystemV/CST6CDT -6 SystemV C%sT\n"
    "Z SystemV/MST7MDT -7 SystemV M%sT\n"
    "Z SystemV/PST8PDT -8 SystemV P%sT\n"
    "Z SystemV/YST9YDT -9 SystemV Y%sT\n"
    "Z SystemV/AST4 -4 - AST\n"
    "Z SystemV/EST5 -5 - EST\n"
    "Z SystemV/CST6 -6 - CST\n"
    "Z SystemV/MST7 -7 - MST\n"
    "Z SystemV/PST8 -8 - PST\n"
    "Z SystemV/YST9 -9 - YST\n"
    "Z SystemV/HST10 -10 - HST\n";

/* Names in the database that java.time does not offer as region IDs. */
static const char *const EXCLUDED[] = { "EST", "HST", "MST", "ROC", "GMT+0", "GMT-0" };

/* --- en_US zone names, generated from the JDK 23 runtime --------- */

typedef struct {
    bool full;            /* a long name (zzzz, vvvv), else short (z, v) */
    const char *name;     /* UTF-8 */
    const char *region;
    int type;             /* 0 standard, 1 daylight, 2 generic */
} zone_name;

static const zone_name NAMES[] = {
#include "tzdb_names.inc"
};

#define NAME_COUNT (sizeof(NAMES) / sizeof(NAMES[0]))

/* --- the database ------------------------------------------------ */

typedef struct {
    int32_t from, to;     /* years */
    int8_t month;         /* 1-12 */
    int8_t dkind;         /* DAY_FIXED, DAY_LAST, DAY_GE, DAY_LE */
    int8_t dow;           /* 0 Sunday - 6 Saturday */
    int8_t day;
    char at_kind;         /* 'w' wall, 's' standard, 'u' universal */
    int32_t at;           /* seconds after midnight */
    int32_t save;
} tz_rule;

enum { DAY_FIXED, DAY_LAST, DAY_GE, DAY_LE };

typedef struct {
    const char *name;
    tz_rule *rules;
    size_t n, cap;
} tz_ruleset;

typedef struct {
    int32_t stdoff;
    const char *rule_name;    /* the rule set's name, or NULL */
    int ruleset;          /* -1 for a fixed saving */
    int32_t save;         /* the fixed saving */
    bool has_until;
    char until_kind;
    int64_t until;        /* local seconds, read as until_kind says */
} tz_line;

typedef struct {
    int64_t at;           /* the instant */
    int32_t before, after;
} tz_trans;

typedef struct {
    const char *name;
    tz_line *lines;
    size_t n, cap;
    /* Worked out on first use, under build_lock. */
    bool built;
    int32_t initial;      /* the offset before the first transition */
    tz_trans *trans;
    size_t ntrans;
    int32_t last_save;    /* the saving at the end of LAST_YEAR */
} tz_zone;

typedef struct {
    const char *id;
    uint16_t *u;
    size_t nu;
    int zone;
} tz_region;

static tz_ruleset *rulesets;
static size_t nrulesets, cap_rulesets;
static tz_zone *zones;
static size_t nzones, cap_zones;
static tz_region *regions;
static size_t nregions;

typedef struct { uint16_t *u; size_t nu; int region; } name_units;
static name_units *names;

static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t build_lock = PTHREAD_MUTEX_INITIALIZER;

static void *grow(void *p, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap) return p;
    size_t cap2 = *cap ? *cap * 2 : 16;
    while (cap2 < need) cap2 *= 2;
    void *np = AUSSOM_ALLOC(cap2 * size);
    if (*cap) memcpy(np, p, *cap * size);
    *cap = cap2;
    return np;
}

/* --- reading tzdata.zi ------------------------------------------- */

#define MAX_FIELDS 12

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* The index of the one word in `words` that `s` abbreviates, as zic
 * reads month and day names, or -1. */
static int lookup(const char *s, const char *const *words, int n)
{
    size_t len = strlen(s);
    int found = -1;
    for (int i = 0; i < n; i++) {
        size_t k = 0;
        while (k < len && words[i][k] != '\0' && lower(s[k]) == lower(words[i][k])) k++;
        if (k != len) continue;
        if (words[i][k] == '\0') return i;     /* an exact match wins */
        if (found >= 0) return -1;             /* ambiguous */
        found = i;
    }
    return found;
}

static const char *const MONTHS[] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};
static const char *const DAYS[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

/* A time such as 2, -0:30, 1:30:15 or 2s. The suffix, if any, goes to
 * *kind. */
static int32_t read_time(const char *s, char *kind)
{
    *kind = 'w';
    if (strcmp(s, "-") == 0) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int32_t part[3] = { 0, 0, 0 };
    int np = 0;
    while (np < 3) {
        int32_t v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        part[np++] = v;
        if (*s != ':') break;
        s++;
    }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') s++; }
    char k = lower(*s);
    if (k == 's') *kind = 's';
    else if (k == 'u' || k == 'g' || k == 'z') *kind = 'u';
    return sign * (part[0] * 3600 + part[1] * 60 + part[2]);
}

static int32_t read_year(const char *s, int32_t prev)
{
    if (lower(s[0]) == 'm' && lower(s[1]) == 'i') return INT32_MIN;
    if (lower(s[0]) == 'm' && lower(s[1]) == 'a') return INT32_MAX;
    if (lower(s[0]) == 'o') return prev;
    return (int32_t)strtol(s, NULL, 10);
}

/* A day such as 15, lastSu, Su>=8 or Su<=25. */
static void read_day(const char *s, int8_t *kind, int8_t *dow, int8_t *day)
{
    *dow = 0;
    *day = 1;
    if (strncmp(s, "last", 4) == 0) {
        *kind = DAY_LAST;
        *dow = (int8_t)lookup(s + 4, DAYS, 7);
        return;
    }
    const char *op = strpbrk(s, "<>");
    if (op == NULL) {
        *kind = DAY_FIXED;
        *day = (int8_t)atoi(s);
        return;
    }
    char name[16];
    size_t n = (size_t)(op - s) < sizeof(name) - 1 ? (size_t)(op - s) : sizeof(name) - 1;
    memcpy(name, s, n);
    name[n] = '\0';
    *dow = (int8_t)lookup(name, DAYS, 7);
    *kind = (*op == '>') ? DAY_GE : DAY_LE;
    *day = (int8_t)atoi(op + 2);
}

/* The epoch day a rule or until date falls on. */
static int64_t day_of(int64_t year, int month, int kind, int dow, int day)
{
    if (kind == DAY_FIXED) return date_days_from_civil(year, month, day);
    if (kind == DAY_LAST) {
        int64_t d = date_days_from_civil(year, month, date_days_in_month(year, month));
        int wd = (int)date_floormod(d + 4, 7);
        return d - ((wd - dow + 7) % 7);
    }
    int64_t d = date_days_from_civil(year, month, day);
    int wd = (int)date_floormod(d + 4, 7);
    if (kind == DAY_GE) return d + ((dow - wd + 7) % 7);
    return d - ((wd - dow + 7) % 7);
}

static int ruleset_named(const char *name)
{
    for (size_t i = 0; i < nrulesets; i++) {
        if (strcmp(rulesets[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static void add_rule(char **f)
{
    int rs = ruleset_named(f[1]);
    if (rs < 0) {
        rulesets = grow(rulesets, &cap_rulesets, nrulesets + 1, sizeof(tz_ruleset));
        memset(&rulesets[nrulesets], 0, sizeof(tz_ruleset));
        rulesets[nrulesets].name = AUSSOM_STRDUP(f[1]);
        rs = (int)nrulesets++;
    }
    tz_ruleset *set = &rulesets[rs];
    set->rules = grow(set->rules, &set->cap, set->n + 1, sizeof(tz_rule));
    tz_rule *r = &set->rules[set->n++];
    r->from = read_year(f[2], 0);
    r->to = read_year(f[3], r->from);
    r->month = (int8_t)(lookup(f[5], MONTHS, 12) + 1);
    read_day(f[6], &r->dkind, &r->dow, &r->day);
    r->at = read_time(f[7], &r->at_kind);
    char ignored;
    r->save = read_time(f[8], &ignored);
}

/* A zone line from its standard offset on: STDOFF RULES FORMAT
 * [UNTIL]. Rule names are looked up once the whole file is read. */
static void add_line(tz_zone *z, char **f, int nf)
{
    z->lines = grow(z->lines, &z->cap, z->n + 1, sizeof(tz_line));
    tz_line *ln = &z->lines[z->n];
    memset(ln, 0, sizeof(*ln));
    char ignored;
    ln->stdoff = read_time(f[0], &ignored);
    ln->ruleset = -1;
    const char *rules = f[1];
    if (strcmp(rules, "-") == 0) {
        ln->save = 0;
    } else if ((rules[0] >= '0' && rules[0] <= '9') || (rules[0] == '-' && rules[1] >= '0' && rules[1] <= '9')) {
        ln->save = read_time(rules, &ignored);
    } else {
        ln->rule_name = AUSSOM_STRDUP(rules);
    }
    if (nf > 3) {
        ln->has_until = true;
        int64_t year = strtol(f[3], NULL, 10);
        int month = nf > 4 ? lookup(f[4], MONTHS, 12) + 1 : 1;
        int8_t kind = DAY_FIXED, dow = 0, day = 1;
        if (nf > 5) read_day(f[5], &kind, &dow, &day);
        int32_t at = 0;
        ln->until_kind = 'w';
        if (nf > 6) at = read_time(f[6], &ln->until_kind);
        ln->until = day_of(year, month, kind, dow, day) * 86400 + at;
    }
    z->n++;
}

typedef struct { const char *name, *target; } tz_link;

/* Reads zi text into the rule sets and zones, collecting links. */
static void read_zi(const char *text, tz_link **links, size_t *nlinks, size_t *cap_links)
{
    tz_zone *cur = NULL;
    const char *p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = AUSSOM_ALLOC_ATOMIC(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        p += len + (eol ? 1 : 0);
        char *hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';

        char *f[MAX_FIELDS];
        int nf = 0;
        for (char *q = line; *q != '\0' && nf < MAX_FIELDS;) {
            while (*q == ' ' || *q == '\t') *q++ = '\0';
            if (*q == '\0') break;
            f[nf++] = q;
            while (*q != '\0' && *q != ' ' && *q != '\t') q++;
        }
        if (nf == 0) continue;

        if (strcmp(f[0], "R") == 0 && nf >= 10) {
            cur = NULL;
            add_rule(f);
        } else if (strcmp(f[0], "L") == 0 && nf >= 3) {
            cur = NULL;
            *links = grow(*links, cap_links, *nlinks + 1, sizeof(tz_link));
            (*links)[*nlinks].target = AUSSOM_STRDUP(f[1]);
            (*links)[*nlinks].name = AUSSOM_STRDUP(f[2]);
            (*nlinks)++;
        } else if (strcmp(f[0], "Z") == 0 && nf >= 5) {
            zones = grow(zones, &cap_zones, nzones + 1, sizeof(tz_zone));
            cur = &zones[nzones];
            memset(cur, 0, sizeof(*cur));
            cur->name = AUSSOM_STRDUP(f[1]);
            nzones++;
            add_line(cur, f + 2, nf - 2);
        } else if (cur != NULL && nf >= 3) {
            add_line(cur, f, nf);
        }
    }
}

static int zone_named(const char *name)
{
    for (size_t i = 0; i < nzones; i++) {
        if (strcmp(zones[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int compare_regions(const void *a, const void *b)
{
    return strcmp(((const tz_region *)a)->id, ((const tz_region *)b)->id);
}

static void add_region(const char *id, int zone, size_t *cap)
{
    for (size_t i = 0; i < sizeof(EXCLUDED) / sizeof(EXCLUDED[0]); i++) {
        if (strcmp(id, EXCLUDED[i]) == 0) return;
    }
    regions = grow(regions, cap, nregions + 1, sizeof(tz_region));
    tz_region *r = &regions[nregions++];
    r->id = id;
    r->u = av_text_to_units(id, strlen(id), &r->nu);
    r->zone = zone;
}

/* --- matching IDs and names --------------------------------------
 *
 * java.time reads a zone ID or name by walking a prefix tree of every
 * key it accepts. The walk is greedy: it follows the branch the text
 * leads into for as long as the text matches the branch, and returns
 * the key it stops at, if the place it stops is a key. It does not back
 * up, so "Etc/GMT+" matches nothing although "Etc/GMT" is a key: the
 * walk went on into the branch for Etc/GMT+1 to Etc/GMT+12.
 *
 * Here the tree is a sorted array of keys. A branch for the text so far
 * plus its next character is the range of keys starting that way, and
 * the place the branch leads to is the common prefix of that range. */

typedef struct {
    const uint16_t *u;
    size_t nu;
    int region;
    int type;             /* the kind of name, or -1 for an ID */
} tz_key;

typedef struct {
    tz_key *k;
    size_t n;
} key_set;

/* IDs only (VV), IDs and short names (z, v), IDs and long names. */
static key_set key_sets[3];

static int compare_units(const uint16_t *a, size_t na, const uint16_t *b, size_t nb)
{
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return (na > nb) - (na < nb);
}

static int compare_keys(const void *a, const void *b)
{
    const tz_key *x = a, *y = b;
    int c = compare_units(x->u, x->nu, y->u, y->nu);
    /* An ID sorts before a name that is the same text. */
    if (c == 0) c = (x->type >= 0) - (y->type >= 0);
    return c;
}

static void build_keys(key_set *ks, int names_for)
{
    size_t cap = nregions + NAME_COUNT, n = 0;
    tz_key *k = AUSSOM_ALLOC(cap * sizeof(tz_key));
    for (size_t i = 0; i < nregions; i++) {
        k[n++] = (tz_key){ regions[i].u, regions[i].nu, (int)i, -1 };
    }
    for (size_t i = 0; names_for != 0 && i < NAME_COUNT; i++) {
        if (NAMES[i].full != (names_for == 2) || names[i].region < 0) continue;
        k[n++] = (tz_key){ names[i].u, names[i].nu, names[i].region, NAMES[i].type };
    }
    qsort(k, n, sizeof(tz_key), compare_keys);
    /* Where a name is also an ID, the name is the key's value. */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (w > 0 && compare_units(k[w - 1].u, k[w - 1].nu, k[i].u, k[i].nu) == 0) k[w - 1] = k[i];
        else k[w++] = k[i];
    }
    ks->k = k;
    ks->n = w;
}

/* The region with this ID, or -1, once the regions are sorted. */
static int region_named(const char *id)
{
    size_t a = 0, b = nregions;
    while (a < b) {
        size_t m = (a + b) / 2;
        int c = strcmp(regions[m].id, id);
        if (c == 0) return (int)m;
        if (c < 0) a = m + 1; else b = m;
    }
    return -1;
}

static void load(void)
{
    tz_link *links = NULL;
    size_t nlinks = 0, cap_links = 0;
    read_zi(aussom_tzdata_zi, &links, &nlinks, &cap_links);
    read_zi(SYSTEMV_ZI, &links, &nlinks, &cap_links);

    for (size_t i = 0; i < nzones; i++) {
        for (size_t k = 0; k < zones[i].n; k++) {
            tz_line *ln = &zones[i].lines[k];
            if (ln->rule_name != NULL) ln->ruleset = ruleset_named(ln->rule_name);
        }
    }

    size_t cap = 0;
    for (size_t i = 0; i < nzones; i++) add_region(zones[i].name, (int)i, &cap);
    for (size_t i = 0; i < nlinks; i++) {
        /* A link may name another link. */
        const char *target = links[i].target;
        int zone = zone_named(target);
        for (int hops = 0; zone < 0 && hops < 8; hops++) {
            size_t k = 0;
            while (k < nlinks && strcmp(links[k].name, target) != 0) k++;
            if (k == nlinks) break;
            target = links[k].target;
            zone = zone_named(target);
        }
        if (zone >= 0) add_region(links[i].name, zone, &cap);
    }
    qsort(regions, nregions, sizeof(tz_region), compare_regions);

    names = AUSSOM_ALLOC(NAME_COUNT * sizeof(name_units));
    for (size_t i = 0; i < NAME_COUNT; i++) {
        names[i].u = av_text_to_units(NAMES[i].name, strlen(NAMES[i].name), &names[i].nu);
        names[i].region = region_named(NAMES[i].region);
    }
    for (int m = 0; m < 3; m++) build_keys(&key_sets[m], m);
}

static void ensure_loaded(void)
{
    pthread_once(&load_once, load);
}

/* --- transitions ------------------------------------------------- */

typedef struct {
    int64_t local;        /* local seconds, read as kind says */
    char kind;
    int32_t save;
} tz_event;

static int compare_events(const void *a, const void *b)
{
    const tz_event *x = a, *y = b;
    return (x->local > y->local) - (x->local < y->local);
}

/* The rule changes of a rule set in years [y0, y1], in order. */
static tz_event *events(const tz_ruleset *set, int64_t y0, int64_t y1, size_t *n)
{
    size_t cap = 0, k = 0;
    tz_event *ev = NULL;
    for (int64_t y = y0; y <= y1; y++) {
        for (size_t i = 0; i < set->n; i++) {
            const tz_rule *r = &set->rules[i];
            if (y < r->from || y > r->to) continue;
            ev = grow(ev, &cap, k + 1, sizeof(tz_event));
            ev[k].local = day_of(y, r->month, r->dkind, r->dow, r->day) * 86400 + r->at;
            ev[k].kind = r->at_kind;
            ev[k].save = r->save;
            k++;
        }
    }
    if (k > 1) qsort(ev, k, sizeof(tz_event), compare_events);
    *n = k;
    return ev;
}

/* An event's instant, given the offsets in force just before it. */
static int64_t event_instant(int64_t local, char kind, int32_t stdoff, int32_t save)
{
    if (kind == 'u') return local;
    if (kind == 's') return local - stdoff;
    return local - stdoff - save;
}

static int64_t year_of(int64_t instant)
{
    int64_t y;
    int m, d;
    date_civil_from_days(date_floordiv(instant, 86400), &y, &m, &d);
    return y;
}

typedef struct {
    tz_trans *t;
    size_t n, cap;
} trans_list;

/* Adds a change at `at` to `after`, merging changes at one instant and
 * dropping any that leave the offset as it was. */
static void add_trans(trans_list *l, int64_t at, int32_t before, int32_t after)
{
    if (l->n > 0 && l->t[l->n - 1].at >= at) {
        tz_trans *last = &l->t[l->n - 1];
        last->after = after;
        if (last->before == last->after) l->n--;
        return;
    }
    if (before == after) return;
    l->t = grow(l->t, &l->cap, l->n + 1, sizeof(tz_trans));
    l->t[l->n].at = at;
    l->t[l->n].before = before;
    l->t[l->n].after = after;
    l->n++;
}

static void build(tz_zone *z)
{
    trans_list l = { NULL, 0, 0 };
    int64_t start = INT64_MIN;
    int32_t cur = 0, save = 0, prev_std = 0;
    for (size_t i = 0; i < z->n; i++) {
        const tz_line *ln = &z->lines[i];
        int32_t std = ln->stdoff;
        bool last = (i + 1 == z->n);
        if (ln->ruleset < 0) {
            save = ln->save;
            if (i == 0) z->initial = std + save;
            else add_trans(&l, start, cur, std + save);
            cur = std + save;
        } else {
            const tz_ruleset *set = &rulesets[ln->ruleset];
            int64_t y0 = INT32_MAX;
            for (size_t k = 0; k < set->n; k++) if (set->rules[k].from < y0) y0 = set->rules[k].from;
            if (y0 < -10000) y0 = -10000;
            int64_t y1 = ln->has_until ? year_of(ln->until) + 1 : LAST_YEAR;
            if (y1 > LAST_YEAR) y1 = LAST_YEAR;
            size_t ne;
            tz_event *ev = events(set, y0, y1, &ne);
            /* The saving in force at the start is that of the last change
             * at or before it, timed by the offsets in force before the
             * start. */
            int32_t prev_save = save;
            save = 0;
            size_t k = 0;
            while (k < ne && (i == 0 ? false :
                   event_instant(ev[k].local, ev[k].kind, prev_std, prev_save) <= start)) {
                save = ev[k].save;
                k++;
            }
            if (i == 0) z->initial = std + save;
            else add_trans(&l, start, cur, std + save);
            cur = std + save;
            for (; k < ne; k++) {
                int64_t at = event_instant(ev[k].local, ev[k].kind, std, save);
                if (ln->has_until && at >= event_instant(ln->until, ln->until_kind, std, save)) break;
                save = ev[k].save;
                add_trans(&l, at, cur, std + save);
                cur = std + save;
            }
        }
        if (!last) start = event_instant(ln->until, ln->until_kind, std, save);
        prev_std = std;
    }
    z->trans = l.t;
    z->ntrans = l.n;
    z->last_save = save;
    z->built = true;
}

static tz_zone *zone_of(int region)
{
    tz_zone *z = &zones[regions[region].zone];
    pthread_mutex_lock(&build_lock);
    if (!z->built) build(z);
    pthread_mutex_unlock(&build_lock);
    return z;
}

/* The changes after LAST_YEAR, which repeat the last line's rules, for
 * years [y0, y1] (each after LAST_YEAR). */
static size_t late_trans(const tz_zone *z, int64_t y0, int64_t y1, tz_trans *out, size_t max)
{
    const tz_line *ln = &z->lines[z->n - 1];
    if (ln->ruleset < 0) return 0;
    size_t ne;
    tz_event *ev = events(&rulesets[ln->ruleset], y0, y1, &ne);
    int32_t std = ln->stdoff, save = z->last_save;
    size_t k = 0;
    for (size_t i = 0; i < ne; i++) {
        int64_t at = event_instant(ev[i].local, ev[i].kind, std, save);
        if (ev[i].save != save && k < max) {
            out[k].at = at;
            out[k].before = std + save;
            out[k].after = std + ev[i].save;
            k++;
        }
        save = ev[i].save;
    }
    return k;
}

/* The changes in [lo, hi] and the offset in force at lo. */
static size_t trans_between(const tz_zone *z, int64_t lo, int64_t hi, tz_trans *out, size_t max,
                            int32_t *at_lo)
{
    /* The first explicit change after lo. */
    size_t a = 0, b = z->ntrans;
    while (a < b) {
        size_t m = (a + b) / 2;
        if (z->trans[m].at <= lo) a = m + 1; else b = m;
    }
    *at_lo = a == 0 ? z->initial : z->trans[a - 1].after;
    size_t k = 0;
    for (size_t i = a; i < z->ntrans && z->trans[i].at <= hi && k < max; i++) out[k++] = z->trans[i];

    int64_t ylo = year_of(lo) - 1, yhi = year_of(hi) + 1;
    if (yhi <= LAST_YEAR) return k;
    if (ylo <= LAST_YEAR) ylo = LAST_YEAR + 1;
    tz_trans late[16];
    size_t nl = late_trans(z, ylo, yhi, late, 16);
    for (size_t i = 0; i < nl; i++) {
        if (late[i].at <= lo) *at_lo = late[i].after;
        else if (late[i].at <= hi && k < max) out[k++] = late[i];
    }
    return k;
}

tz_local_kind tz_local_offsets(int region, int64_t local, int32_t *earlier, int32_t *later)
{
    ensure_loaded();
    *earlier = *later = 0;
    if (region < 0 || (size_t)region >= nregions) return TZ_NORMAL;
    tz_zone *z = zone_of(region);
    /* Offsets stay within a day of zero, so the instants local time
     * could mean lie within two days of it. */
    tz_trans t[32];
    int32_t first;
    size_t nt = trans_between(z, local - 2 * 86400, local + 2 * 86400, t, 32, &first);

    int32_t valid[33];
    size_t nv = 0;
    int32_t off = first;
    for (size_t i = 0; i <= nt; i++) {
        /* `off` is in force from the previous change up to t[i]. */
        int64_t from = i == 0 ? INT64_MIN : t[i - 1].at;
        int64_t to = i == nt ? INT64_MAX : t[i].at;
        int64_t inst = local - off;
        if (inst >= from && inst < to) valid[nv++] = off;
        if (i < nt) off = t[i].after;
    }
    if (nv == 1) {
        *earlier = *later = valid[0];
        return TZ_NORMAL;
    }
    if (nv >= 2) {
        *earlier = valid[0];
        *later = valid[nv - 1];
        return TZ_OVERLAP;
    }
    for (size_t i = 0; i < nt; i++) {
        if (local >= t[i].at + t[i].before && local < t[i].at + t[i].after) {
            *earlier = t[i].before;
            *later = t[i].after;
            return TZ_GAP;
        }
    }
    *earlier = *later = first;
    return TZ_NORMAL;
}

/* --- lookups ----------------------------------------------------- */

int tz_region_named(const char *id)
{
    ensure_loaded();
    return region_named(id);
}

const char *tz_region_id(int region)
{
    ensure_loaded();
    if (region < 0 || (size_t)region >= nregions) return "";
    return regions[region].id;
}

/* The first key in [lo, hi) not before t[0..n) in order. */
static size_t lower_key(const key_set *ks, size_t lo, size_t hi, const uint16_t *t, size_t n)
{
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (compare_units(ks->k[m].u, ks->k[m].nu, t, n) < 0) lo = m + 1; else hi = m;
    }
    return lo;
}

static bool key_has_prefix(const tz_key *k, const uint16_t *t, size_t n)
{
    return k->nu >= n && memcmp(k->u, t, n * sizeof(uint16_t)) == 0;
}

int tz_match(int names_for, const uint16_t *t, size_t n, size_t *len, int *type)
{
    ensure_loaded();
    const key_set *ks = &key_sets[names_for];
    size_t at = 0;            /* the text matched so far */
    size_t lo = 0, hi = ks->n;
    while (at < n) {
        /* The keys that go on with t[at]. */
        size_t a = lower_key(ks, lo, hi, t, at + 1);
        size_t b = a;
        while (b < hi && key_has_prefix(&ks->k[b], t, at + 1)) b++;
        if (a == b) break;
        /* The branch leads to the common prefix of those keys. */
        const tz_key *first = &ks->k[a], *last = &ks->k[b - 1];
        size_t q = at + 1;
        while (q < first->nu && q < last->nu && first->u[q] == last->u[q]) q++;
        if (q > n || memcmp(first->u, t, q * sizeof(uint16_t)) != 0) break;
        at = q;
        lo = a;
        hi = b;
    }
    /* The key the walk stopped at, if it is one. */
    size_t i = lower_key(ks, lo, hi, t, at);
    if (at == 0 || i >= hi || ks->k[i].nu != at || !key_has_prefix(&ks->k[i], t, at)) {
        *len = 0;
        return -1;
    }
    *len = at;
    *type = ks->k[i].type;
    return ks->k[i].region;
}
