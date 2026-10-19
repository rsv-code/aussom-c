/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The `string` type (com.aussom.types.AussomString). A working slice
 * of the stdlib string methods; the full set lands in Phase 3b.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/memstream.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"
#include "unicode.h"

/* Strings are compared, searched and indexed by their UTF-16 code
 * units, as java.lang.String does it. */

static const char *self_str(av_type *self) { return av_string_get((av_string *)self); }
static const char *arg_str(av_list *a, size_t i) { const char *s = av_list_get_string(a, i); return (s != NULL) ? s : ""; }
static int64_t arg_int(av_list *a, size_t i) { int64_t v = 0; av_list_get_int(a, i, &v); return v; }

static const uint16_t *self_units(av_type *self, size_t *n)
{
    return av_string_units((av_string *)self, n);
}

/* The units of string argument i; an argument that is not a string
 * reads as "". */
static const uint16_t *arg_units(av_list *a, size_t i, size_t *n)
{
    av_type *v = i < av_list_size(a) ? av_list_get(a, i) : NULL;
    if (v == NULL || v->type != C_TYPE_STRING) { *n = 0; return NULL; }
    return av_string_units((av_string *)v, n);
}

static av_type *ustr(const uint16_t *u, size_t n)
{
    return &av_string_new_units(u, n)->base.base;
}

static av_type *str_length(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_int_new((int64_t)av_string_length((av_string *)self))->base.base;
}

static av_type *str_isEmpty(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return &av_bool_new(av_string_length((av_string *)self) == 0)->base.base;
}

static av_type *str_toUpper(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    size_t n, rn;
    const uint16_t *u = self_units(self, &n);
    uint16_t *r = uni_to_upper(u, n, &rn);
    return ustr(r, rn);
}

static av_type *str_toLower(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    size_t n, rn;
    const uint16_t *u = self_units(self, &n);
    uint16_t *r = uni_to_lower(u, n, &rn);
    return ustr(r, rn);
}

static av_type *str_trim(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    size_t n;
    const uint16_t *u = self_units(self, &n);
    /* String.trim: every unit at or below ' ', not just whitespace. */
    size_t start = 0, end = n;
    while (start < end && u[start] <= ' ') start++;
    while (end > start && u[end - 1] <= ' ') end--;
    return ustr(u + start, end - start);
}

/* --- format --------------------------------------------------------
 * The Aussom-side `format(...)` collects its variadic args into a list
 * and forwards to `_format(list Args)`, which is this extern. So our
 * single argument is the list of real format arguments.
 *
 * Placeholders: {} next positional, {N} positional by index (reusable),
 * {name} a key in the first map argument. '{{' and '}}' are literal
 * braces. Scalars insert their plain str() (a string is NOT quoted);
 * lists and maps render as compact JSON rather than the multi-line
 * debug form. */

/* Character.isDigit on each unit: any Unicode decimal digit. */
static bool all_digits(const uint16_t *s, size_t n)
{
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (uni_digit(s[i]) < 0) return false;
    }
    return true;
}

typedef struct { uint16_t *u; size_t n, cap; } ubuf;

static void ub_add(ubuf *b, const uint16_t *u, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 32;
        while (cap < b->n + n) cap *= 2;
        uint16_t *nu = AUSSOM_ALLOC_ATOMIC(cap * sizeof(uint16_t));
        if (b->n > 0) memcpy(nu, b->u, b->n * sizeof(uint16_t));
        b->u = nu;
        b->cap = cap;
    }
    if (n > 0) memcpy(b->u + b->n, u, n * sizeof(uint16_t));
    b->n += n;
}

static void ub_add_text(ubuf *b, const char *text)
{
    size_t n;
    uint16_t *u = av_text_to_units(text, strlen(text), &n);
    ub_add(b, u, n);
}

static av_type *fmt_error(const char *m)
{
    return &av_exception_new(m)->base.base;
}

static av_type *str_format(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_list *fargs = NULL;
    if (av_list_size(args) > 0 && av_list_get(args, 0)->type == C_TYPE_LIST) {
        fargs = (av_list *)av_list_get(args, 0);
    } else {
        fargs = args;
    }
    size_t nargs = av_list_size(fargs);

    /* First map argument, if any, supplies named placeholders. */
    av_map *named = NULL;
    for (size_t i = 0; i < nargs; i++) {
        if (av_list_get(fargs, i)->type == C_TYPE_MAP) {
            named = (av_map *)av_list_get(fargs, i);
            break;
        }
    }

    size_t len;
    const uint16_t *s = self_units(self, &len);
    ubuf out = { NULL, 0, 0 };
    char m[192];

    size_t auto_index = 0;
    size_t i = 0;
    while (i < len) {
        uint16_t ch = s[i];
        if (ch == '{') {
            if (i + 1 < len && s[i + 1] == '{') { uint16_t c = '{'; ub_add(&out, &c, 1); i += 2; continue; }
            size_t close = i;
            while (close < len && s[close] != '}') close++;
            if (close == len) {
                snprintf(m, sizeof(m), "string.format(): Unmatched '{' at index %zu.", i);
                return fmt_error(m);
            }
            /* The key, trimmed as String.trim does it. */
            size_t ks = i + 1, ke = close;
            while (ks < ke && s[ks] <= ' ') ks++;
            while (ke > ks && s[ke - 1] <= ' ') ke--;
            size_t klen = ke - ks;
            const char *key = av_units_to_text(s + ks, klen, NULL);

            av_type *val = NULL;
            if (klen == 0) {
                if (auto_index >= nargs) {
                    snprintf(m, sizeof(m),
                        "string.format(): Not enough arguments for placeholder number %zu.",
                        auto_index);
                    return fmt_error(m);
                }
                val = av_list_get(fargs, auto_index++);
            } else if (all_digits(s + ks, klen)) {
                /* Integer.parseInt: leading zeros are fine, and a value
                 * past an int gets its own message, quoting the key. */
                int64_t idx = 0;
                bool too_big = false;
                for (size_t k = ks; k < ke && !too_big; k++) {
                    idx = idx * 10 + uni_digit(s[k]);
                    if (idx > INT32_MAX) too_big = true;
                }
                if (too_big) {
                    size_t n = strlen(key) + 80;
                    char *mk = AUSSOM_ALLOC_ATOMIC(n);
                    snprintf(mk, n, "string.format(): Argument index '%s' out of range.", key);
                    return fmt_error(mk);
                }
                if ((size_t)idx >= nargs) {
                    snprintf(m, sizeof(m),
                        "string.format(): Argument index %lld out of range.", (long long)idx);
                    return fmt_error(m);
                }
                val = av_list_get(fargs, (size_t)idx);
            } else {
                val = (named != NULL) ? av_map_get(named, key) : NULL;
                if (val == NULL) {
                    size_t n = strlen(key) + 96;
                    char *mk = AUSSOM_ALLOC_ATOMIC(n);
                    snprintf(mk, n,
                        "string.format(): No value provided for named placeholder '%s'.", key);
                    return fmt_error(mk);
                }
            }

            if (val->type == C_TYPE_LIST || val->type == C_TYPE_MAP) {
                av_type *json = av_to_json(val);
                if (av_is_ex(json)) return json;
                ub_add_text(&out, av_str(json));
            } else {
                ub_add_text(&out, av_str(val));
            }
            i = close + 1;
        } else if (ch == '}') {
            if (i + 1 < len && s[i + 1] == '}') { uint16_t c = '}'; ub_add(&out, &c, 1); i += 2; continue; }
            snprintf(m, sizeof(m),
                "string.format(): Single '}' at index %zu; use '}}' for a literal '}'.", i);
            return fmt_error(m);
        } else {
            ub_add(&out, &ch, 1);
            i++;
        }
    }
    return ustr(out.u, out.n);
}

static av_type *str_substr(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n;
    const uint16_t *u = self_units(self, &n);
    int64_t len = (int64_t)n;
    int64_t start = arg_int(args, 0);
    av_type *err = stdlib_check_int_range("string.substr", start);
    if (err != NULL) return err;
    int64_t end = len;
    if (av_list_size(args) > 1 && av_list_get(args, 1)->type == C_TYPE_INT) {
        end = arg_int(args, 1);
        err = stdlib_check_int_range("string.substr", end);
        if (err != NULL) return err;
    }
    if (start < 0 || start > len || end < start || end > len) {
        /* String.substring throws; callExtern reports it as thrown. */
        char m[160];
        snprintf(m, sizeof(m),
            "java.lang.StringIndexOutOfBoundsException: Range [%lld, %lld) out of bounds for length %lld",
            (long long)start, (long long)end, (long long)len);
        return &av_exception_new_thrown(m)->base.base;
    }
    return ustr(u + start, (size_t)(end - start));
}

static av_type *str_charAt(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n;
    const uint16_t *u = self_units(self, &n);
    int64_t i = arg_int(args, 0);
    av_type *err = stdlib_check_int_range("string.charAt", i);
    if (err != NULL) return err;
    if (i < 0 || i >= (int64_t)n) {
        /* AussomString.charAt throws an aussomException. */
        char m[80];
        snprintf(m, sizeof(m), "Index %lld out of bounds.", (long long)i);
        return &av_exception_new_thrown_aussom(m)->base.base;
    }
    return ustr(u + i, 1);
}

static av_type *str_contains(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, nn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *nd = arg_units(args, 0, &nn);
    return &av_bool_new(uni_index_of(u, n, nd, nn, 0) >= 0)->base.base;
}

static av_type *str_indexOf(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, nn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *nd = arg_units(args, 0, &nn);
    return &av_int_new(uni_index_of(u, n, nd, nn, 0))->base.base;
}

static bool units_eq(const uint16_t *a, size_t na, const uint16_t *b, size_t nb)
{
    return na == nb && (na == 0 || memcmp(a, b, na * sizeof(uint16_t)) == 0);
}

static av_type *str_equals(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, nn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *o = arg_units(args, 0, &nn);
    return &av_bool_new(units_eq(u, n, o, nn))->base.base;
}

static av_type *str_startsWith(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, pn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *p = arg_units(args, 0, &pn);
    return &av_bool_new(pn <= n && units_eq(u, pn, p, pn))->base.base;
}

static av_type *str_endsWith(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, pn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *p = arg_units(args, 0, &pn);
    return &av_bool_new(pn <= n && units_eq(u + n - pn, pn, p, pn))->base.base;
}

/* Literal (non-regex) replace-all, matching AussomString.replace. An
 * empty target puts the replacement before every unit and at the end,
 * splitting surrogate pairs as Java does. */
static av_type *str_replace(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, fl, tl;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *from = arg_units(args, 0, &fl);
    const uint16_t *to = arg_units(args, 1, &tl);
    ubuf out = { NULL, 0, 0 };
    if (fl == 0) {
        for (size_t i = 0; i < n; i++) {
            ub_add(&out, to, tl);
            ub_add(&out, u + i, 1);
        }
        ub_add(&out, to, tl);
        return ustr(out.u, out.n);
    }
    size_t i = 0;
    for (;;) {
        int64_t hit = uni_index_of(u, n, from, fl, (int64_t)i);
        if (hit < 0) break;
        ub_add(&out, u + i, (size_t)hit - i);
        ub_add(&out, to, tl);
        i = (size_t)hit + fl;
    }
    ub_add(&out, u + i, n - i);
    return ustr(out.u, out.n);
}

/* AussomString's regex methods catch a PatternSyntaxException and
 * return "string.<method>(): " + its message. A pattern error is the
 * one regex exception with no id; budget and cancel exceptions carry
 * theirs and pass through unchanged. */
static av_type *regex_error(av_type *r, const char *method)
{
    if (r == NULL || r->type != C_TYPE_EXCEPTION) return r;
    av_exception *e = (av_exception *)r;
    const char *id = av_exception_get_id(e);
    if (id != NULL && id[0] != '\0') return r;
    const char *t = av_exception_get_text(e);
    size_t n = strlen(method) + strlen(t) + 16;
    char *m = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(m, n, "string.%s(): %s", method, t);
    return &av_exception_new(m)->base.base;
}

/* split(delim, allowBlanks) -> list of strings.
 *
 * The delimiter is a regular expression, matching the Java engine.
 * This port previously split on a literal delimiter, which quietly
 * disagreed for any delimiter containing a metacharacter: "a.b".split(".")
 * returned ["a", "b"] here and [] there. The regex reading is the
 * documented one, so it wins.
 *
 * allowBlanks controls whether pieces that are empty or all whitespace
 * survive, which is Aussom's own rule layered on top of the split. */
static av_type *str_split(av_type *self, environment *env, av_list *args)
{
    const char *s = self_str(self);
    const char *delim = arg_str(args, 0);
    bool allow_blanks = (av_list_size(args) > 1)
        && av_list_get(args, 1)->type == C_TYPE_BOOL
        && av_bool_get((av_bool *)av_list_get(args, 1));

    av_list *ret = av_list_new();
    av_list *parts;
    av_type *err = aussom_regex_split(env, delim, s, &parts);
    if (err != NULL) return regex_error(err, "split");

    size_t n = av_list_size(parts);
    for (size_t i = 0; i < n; i++) {
        av_type *piece = av_list_get(parts, i);
        size_t pn;
        const uint16_t *pu = av_string_units((av_string *)piece, &pn);
        bool blank = true;
        for (size_t q = 0; q < pn; q++) {
            if (pu[q] > ' ') { blank = false; break; }
        }
        if (allow_blanks || !blank) av_list_add(ret, piece);
    }
    return &ret->base.base;
}

static av_type *str_isBlank(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    /* trim().isEmpty(): every unit at or below ' '. */
    size_t n;
    const uint16_t *u = self_units(self, &n);
    for (size_t i = 0; i < n; i++) {
        if (u[i] > ' ') return &av_bool_new(false)->base.base;
    }
    return &av_bool_new(true)->base.base;
}

static av_type *str_compare(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, on;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *o = arg_units(args, 0, &on);
    return &av_int_new(uni_compare(u, n, o, on))->base.base;
}

static av_type *str_compareICase(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, on;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *o = arg_units(args, 0, &on);
    return &av_int_new(uni_compare_icase(u, n, o, on))->base.base;
}

static av_type *str_equalsICase(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, on;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *o = arg_units(args, 0, &on);
    return &av_bool_new(uni_equals_icase(u, n, o, on))->base.base;
}

/* concat mutates self (this.value += arg) and returns it, matching
 * AussomString.concat. */
static av_type *str_concat(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, on;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *o = arg_units(args, 0, &on);
    ubuf out = { NULL, 0, 0 };
    ub_add(&out, u, n);
    ub_add(&out, o, on);
    av_string_set_units((av_string *)self, out.u, out.n);
    return self;
}

static av_type *str_indexOfStart(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, nn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *nd = arg_units(args, 0, &nn);
    int64_t start = arg_int(args, 1);
    av_type *err = stdlib_check_int_range("string.indexOfStart", start);
    if (err != NULL) return err;
    return &av_int_new(uni_index_of(u, n, nd, nn, start))->base.base;
}

static av_type *str_lastIndexOf(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, nn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *nd = arg_units(args, 0, &nn);
    return &av_int_new(uni_last_index_of(u, n, nd, nn, (int64_t)n))->base.base;
}

static av_type *str_lastIndexOfStart(av_type *self, environment *env, av_list *args)
{
    (void)env;
    size_t n, nn;
    const uint16_t *u = self_units(self, &n);
    const uint16_t *nd = arg_units(args, 0, &nn);
    int64_t start = arg_int(args, 1);
    av_type *err = stdlib_check_int_range("string.lastIndexOfStart", start);
    if (err != NULL) return err;
    return &av_int_new(uni_last_index_of(u, n, nd, nn, start))->base.base;
}

static av_type *str_toJson(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return av_to_json(self);
}

/* --- regex (PCRE2-backed, shared with the regex class) ----------- */

static av_type *str_matches(av_type *self, environment *env, av_list *args)
{ return regex_error(aussom_regex_matches(env, arg_str(args, 0), self_str(self)), "matches"); }

/* A bad replacement string escapes AussomString's replace methods, so
 * it reaches the script as a thrown exception. */
static av_type *str_substitute(av_type *self, environment *env, av_list *args,
                               bool global, const char *method)
{
    const char *ecls = NULL;
    av_type *r = aussom_regex_substitute(env, arg_str(args, 0), arg_str(args, 1),
                                         self_str(self), global, &ecls);
    if (ecls != NULL) {
        const char *t = av_exception_get_text((av_exception *)r);
        size_t n = strlen(ecls) + strlen(t) + 4;
        char *m = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(m, n, "%s: %s", ecls, t);
        return &av_exception_new_thrown(m)->base.base;
    }
    return regex_error(r, method);
}

static av_type *str_replaceRegex(av_type *self, environment *env, av_list *args)
{ return str_substitute(self, env, args, true, "replaceRegex"); }

static av_type *str_replaceFirstRegex(av_type *self, environment *env, av_list *args)
{ return str_substitute(self, env, args, false, "replaceFirstRegex"); }

const aussom_extern_method stdlib_aussom_string_methods[] = {
    { "length", str_length },
    { "isEmpty", str_isEmpty },
    { "isBlank", str_isBlank },
    { "toUpper", str_toUpper },
    { "toLower", str_toLower },
    { "trim", str_trim },
    { "substr", str_substr },
    { "charAt", str_charAt },
    { "contains", str_contains },
    { "indexOf", str_indexOf },
    { "indexOfStart", str_indexOfStart },
    { "lastIndexOf", str_lastIndexOf },
    { "lastIndexOfStart", str_lastIndexOfStart },
    { "equals", str_equals },
    { "equalsICase", str_equalsICase },
    { "compare", str_compare },
    { "compareICase", str_compareICase },
    { "concat", str_concat },
    { "startsWith", str_startsWith },
    { "endsWith", str_endsWith },
    { "replace", str_replace },
    { "split", str_split },
    { "_format", str_format },
    { "toJson", str_toJson },
    { "matches", str_matches },
    { "replaceRegex", str_replaceRegex },
    { "replaceFirstRegex", str_replaceFirstRegex },
    { "pack", stdlib_pack },
    { NULL, NULL }
};
