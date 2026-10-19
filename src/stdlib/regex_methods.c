/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `regex` class (com.aussom.stdlib.ARegex), backed by the
 * vendored PCRE2 (BSD). PCRE2's syntax is Perl-compatible, matching
 * java.util.regex closely (\d, \w, \b, lookarounds, non-greedy, and $1
 * group references in replacements).
 *
 *   match(re, hay)             -> list of the full-match substrings of
 *                                 every non-overlapping match.
 *   replace(re, repl, hay)     -> all matches replaced (replaceAll).
 *   replaceFirst(re, repl, hay)-> first match replaced.
 *
 * Patterns and subjects are read the way java.util.regex reads a
 * UTF-16 string: a surrogate pair is one character, and an unpaired
 * surrogate is a character of its own. They go to PCRE2's 32-bit
 * library in non-UTF mode as one value per character, so `.` and
 * character classes see exactly those characters, and positions are
 * mapped back to UTF-16 indexes. Non-UTF mode also keeps (?i) and \d,
 * \w and \s to ASCII, which is Java's default. PCRE2_NEWLINE_ANY keeps
 * `.` and `$` from crossing \r, U+0085, U+2028 and U+2029, which Java
 * treats as line terminators.
 *
 * Java steps past an empty match by one UTF-16 unit, which can land
 * between the halves of a surrogate pair. A search that starts there
 * sees the two halves as unpaired surrogates, so the subject is
 * re-read with that pair split (see rs_seek).
 */

#define PCRE2_CODE_UNIT_WIDTH 32

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pcre2.h"

#include "aussom/ast_eval.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/engine.h"
#include "aussom/environment.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

/* Exception id raised when a pattern runs past the engine's budget. */
#define REGEX_BUDGET_EXCEEDED_ID "REGEX_BUDGET_EXCEEDED"

/* A string argument, or NULL with *err set, so a script that re-declares
 * this extern class with untyped parameters cannot feed a non-string to
 * the char* casts below. */
static const char *req_str_arg(av_list *a, size_t i, av_type **err)
{
    av_type *v = av_list_get(a, i);
    if (v == NULL || v->type != C_TYPE_STRING) {
        *err = &av_exception_new("regex: expecting string arguments.")->base.base;
        return NULL;
    }
    return av_string_get((av_string *)v);
}

static av_type *regex_exc(int errcode)
{
    PCRE2_UCHAR msg[256];
    int n = pcre2_get_error_message(errcode, msg, sizeof(msg) / sizeof(msg[0]));
    char buf[300];
    size_t o = 0;
    /* PCRE2's messages are ASCII. */
    for (int i = 0; i < n && o + 1 < sizeof(buf); i++) buf[o++] = (char)msg[i];
    buf[o] = '\0';
    return &av_exception_new(buf)->base.base;
}

/* True for the two errors that mean "the budget stopped this", as
 * opposed to a pattern that simply did not match. */
static bool is_budget_error(int rc)
{
    return rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT;
}

/* The budget exception. `limit` is the configured budget, or 0 when
 * none is set and PCRE2's own built-in limit is what stopped the
 * match. The two cases get different wording because the fix differs:
 * a configured budget is raised through policy, while the built-in
 * limit means the pattern's cost has genuinely run away and the
 * pattern is what needs changing.
 *
 * Note this is a place where the C engine bounds something the Java
 * engine does not. java.util.regex has no default limit at all, so
 * without a host-set budget a catastrophic pattern there runs for
 * minutes; PCRE2 always carries one, so the same pattern is refused
 * here in milliseconds even under default policy. */
static av_type *budget_exc(const char *re, uint32_t limit)
{
    char buf[640];
    if (limit > 0) {
        snprintf(buf, sizeof(buf),
                 "Regular expression '%s' exceeded the engine's step budget of "
                 "%lu. Raise or clear the security property '%s' if the pattern "
                 "is intended to cost this much.",
                 re, (unsigned long)limit, ENGINE_LIMIT_REGEX_STEPS_PROP);
    } else {
        snprintf(buf, sizeof(buf),
                 "Regular expression '%s' exceeded the regex engine's built-in "
                 "step limit. The pattern backtracks far more than the length of "
                 "the subject justifies, usually from a nested quantifier such "
                 "as (a+)+ or a backreference. No budget is configured; the "
                 "security property '%s' sets a lower one.",
                 re, ENGINE_LIMIT_REGEX_STEPS_PROP);
    }
    av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
    av_exception_set(e, 0, REGEX_BUDGET_EXCEEDED_ID, buf, buf, AUSSOM_STRDUP(""));
    return &e->base.base;
}

/* The match context every search runs with: the engine's regex budget
 * and the callout that lets a host pause or cancel a match while it
 * runs.
 *
 * The budget is applied as PCRE2's match limit and depth limit, which
 * is what bounds a runaway pattern: an eight-character pattern can
 * otherwise burn a core for a minute on a thirty-character subject,
 * because backtracking cost has nothing to do with subject length.
 * With no budget set, PCRE2's own defaults apply.
 *
 * The Java engine reads the subject through a metered CharSequence, so
 * a host can cancel or pause a match while it runs. PCRE2 has no such
 * hook, so every pattern is compiled with PCRE2_AUTO_CALLOUT (see
 * compile), which calls match_callout before each item of the pattern.
 * That makes every match slower, but a cancel stops even a match the
 * budget would let run for a long time. The budget still counts PCRE2's
 * steps and not characters read, so it stops a runaway pattern at a
 * different point than Java's does. */
typedef struct rctx {
    pcre2_match_context *ctx;
    uint32_t limit;           /* the budget, or 0 when none is set */
    environment *env;
    engine *eng;
    av_type *stop;            /* the cancellation, once a callout saw it */
} rctx;

/* Called before each pattern item. Running is one read and a compare;
 * a pause blocks here until resumed, and a cancel abandons the match
 * with PCRE2_ERROR_CALLOUT, which PCRE2 itself never returns. */
static int match_callout(pcre2_callout_block *cb, void *data)
{
    (void)cb;
    rctx *c = data;
    if (engine_get_control_state(c->eng) == CONTROL_RUNNING) return 0;
    av_type *stop = eval_check_control_env(c->env);
    if (stop == NULL) return 0;
    c->stop = stop;
    return PCRE2_ERROR_CALLOUT;
}

/* Sets up `c` for `env`. c->ctx stays NULL when there is no engine,
 * and PCRE2 then runs with its defaults and no callout. */
static void rctx_open(rctx *c, environment *env)
{
    memset(c, 0, sizeof(*c));
    c->env = env;
    if (env == NULL) return;
    engine *eng = environment_engine(env);
    if (eng == NULL) return;
    pcre2_match_context *ctx = pcre2_match_context_create(NULL);
    if (ctx == NULL) return;
    c->eng = eng;
    pcre2_set_callout(ctx, match_callout, c);
    int64_t steps = engine_get_limits(eng)->regex_steps;
    if (steps > 0) {
        uint32_t lim = (steps > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)steps;
        pcre2_set_match_limit(ctx, lim);
        /* Bounds recursion depth as well as total work. A pattern can
         * blow the backtracking stack without reaching the match
         * limit. */
        pcre2_set_depth_limit(ctx, lim);
        c->limit = lim;
    }
    c->ctx = ctx;
}

static void rctx_close(rctx *c)
{
    if (c->ctx != NULL) pcre2_match_context_free(c->ctx);
    c->ctx = NULL;
}

/* The exception a search result stands for: the cancellation a callout
 * saw, or the budget exception. NULL for a match or a plain failure to
 * match. */
static av_type *rctx_error(const rctx *c, const char *re, int rc)
{
    if (rc == PCRE2_ERROR_CALLOUT && c->stop != NULL) return c->stop;
    if (is_budget_error(rc)) return budget_exc(re, c->limit);
    return NULL;
}

/* Compiles `re`; on failure returns NULL and stores an exception in
 * *err. */
/* A string as java.util.regex sees it: one value per character, a
 * surrogate pair joined and an unpaired surrogate on its own. `at[i]`
 * is the UTF-16 index where character i starts; at[n] is the length. */
typedef struct rsubj {
    const uint16_t *u;
    size_t nu;
    uint32_t *cp;
    size_t *at;
    size_t n;
} rsubj;

static bool is_hi(uint16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
static bool is_lo(uint16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

/* Reads `u` into `rs`, leaving the pair that `split` falls inside (if
 * any) as two unpaired surrogates. */
static void rs_read(rsubj *rs, const uint16_t *u, size_t nu, size_t split)
{
    rs->u = u;
    rs->nu = nu;
    rs->cp = AUSSOM_ALLOC_ATOMIC((nu + 1) * sizeof(uint32_t));
    rs->at = AUSSOM_ALLOC_ATOMIC((nu + 1) * sizeof(size_t));
    size_t k = 0;
    for (size_t i = 0; i < nu;) {
        rs->at[k] = i;
        if (is_hi(u[i]) && i + 1 < nu && is_lo(u[i + 1]) && i + 1 != split) {
            rs->cp[k++] = 0x10000 + (((uint32_t)u[i] - 0xD800) << 10) + ((uint32_t)u[i + 1] - 0xDC00);
            i += 2;
        } else {
            rs->cp[k++] = u[i];
            i++;
        }
    }
    rs->at[k] = nu;
    rs->n = k;
}

static void rs_init(rsubj *rs, const char *text)
{
    size_t nu;
    uint16_t *u = av_text_to_units(text, strlen(text), &nu);
    rs_read(rs, u, nu, (size_t)-1);
}

/* The character index for UTF-16 index `unit`. When `unit` falls
 * inside a pair, the subject is re-read with that pair split first. */
static size_t rs_seek(rsubj *rs, size_t unit)
{
    size_t lo = 0, hi = rs->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rs->at[mid] < unit) lo = mid + 1; else hi = mid;
    }
    if (rs->at[lo] == unit) return lo;
    rs_read(rs, rs->u, rs->nu, unit);
    return rs_seek(rs, unit);
}

static pcre2_code *compile(const char *re, av_type **err)
{
    rsubj p;
    rs_init(&p, re);
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_compile_context *cc = pcre2_compile_context_create(NULL);
    if (cc != NULL) pcre2_set_newline(cc, PCRE2_NEWLINE_ANY);
    /* PCRE2_AUTO_CALLOUT gives match_callout its checkpoints. */
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)p.cp, p.n, PCRE2_AUTO_CALLOUT, &errcode, &erroffset, cc);
    if (cc != NULL) pcre2_compile_context_free(cc);
    if (code == NULL) { *err = regex_exc(errcode); return NULL; }
    *err = NULL;
    return code;
}

/* Runs one search from UTF-16 index `start`. On a match, writes the
 * match's UTF-16 start and end and returns the PCRE2 result. */
static int rs_match(pcre2_code *code, rsubj *rs, size_t start, uint32_t opts,
                    pcre2_match_data *md, pcre2_match_context *ctx,
                    size_t *ms, size_t *me)
{
    size_t from = rs_seek(rs, start);
    int rc = pcre2_match(code, (PCRE2_SPTR)rs->cp, rs->n, from, opts, md, ctx);
    if (rc >= 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        *ms = rs->at[ov[0]];
        *me = rs->at[ov[1]];
    }
    return rc;
}

static av_type *units_str(const uint16_t *u, size_t n)
{
    return &av_string_new_units(u, n)->base.base;
}

static av_type *regex_match(av_type *self, environment *env, av_list *args)
{
    (void)self;
    av_type *aerr = NULL;
    const char *re = req_str_arg(args, 0, &aerr); if (aerr) return aerr;
    const char *hay = req_str_arg(args, 1, &aerr); if (aerr) return aerr;

    av_type *err;
    pcre2_code *code = compile(re, &err);
    if (code == NULL) return err;

    rctx rc_;
    rctx_open(&rc_, env);

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    av_list *out = av_list_new();
    rsubj rs;
    rs_init(&rs, hay);
    size_t start = 0;
    av_type *budget_err = NULL;

    while (start <= rs.nu) {
        /* Control checkpoint between matches, for a scan whose
         * matches are each too short to reach a callout. */
        av_type *stop = eval_check_control_env(env);
        if (stop != NULL) { budget_err = stop; break; }

        size_t ms, me;
        int rc = rs_match(code, &rs, start, 0, md, rc_.ctx, &ms, &me);
        budget_err = rctx_error(&rc_, re, rc);
        if (budget_err != NULL) break;
        if (rc < 0) break;   /* PCRE2_ERROR_NOMATCH or other -> stop */
        av_list_add(out, units_str(rs.u + ms, me - ms));
        start = (me == ms) ? me + 1 : me;   /* step past empty matches */
    }

    pcre2_match_data_free(md);
    rctx_close(&rc_);
    pcre2_code_free(code);
    if (budget_err != NULL) return budget_err;
    return &out->base.base;
}

/* A growable output buffer for the substitute below. */
typedef struct sub_buf {
    uint16_t *p;
    size_t len;
    size_t cap;
} sub_buf;

static void sub_put(sub_buf *b, const uint16_t *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (b->len + n + 1 > cap) cap *= 2;
        uint16_t *np = AUSSOM_ALLOC_ATOMIC(cap * sizeof(uint16_t));
        if (b->len) memcpy(np, b->p, b->len * sizeof(uint16_t));
        b->p = np;
        b->cap = cap;
    }
    if (n > 0) memcpy(b->p + b->len, s, n * sizeof(uint16_t));
    b->len += n;
}

static bool ascii_alnum(uint16_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Matcher.appendReplacement's expansion of `repl` for one match: `\x`
 * is a literal x, `$n` and `${name}` are groups, anything else is
 * copied. A group that did not take part adds nothing. On a bad
 * replacement it returns false and sets *err_class and *err_msg to the
 * Java exception Matcher throws. */
static bool expand_replacement(pcre2_code *code, pcre2_match_data *md,
                               const rsubj *rs, const uint16_t *repl, size_t rl,
                               sub_buf *out, const char **err_class, char **err_msg)
{
    uint32_t groups = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &groups);
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
    uint32_t ovn = pcre2_get_ovector_count(md);
    size_t cur = 0;
    while (cur < rl) {
        uint16_t c = repl[cur];
        if (c == '\\') {
            cur++;
            if (cur == rl) {
                *err_class = "java.lang.IllegalArgumentException";
                *err_msg = AUSSOM_STRDUP("character to be escaped is missing");
                return false;
            }
            sub_put(out, repl + cur, 1);
            cur++;
        } else if (c == '$') {
            cur++;
            if (cur == rl) {
                *err_class = "java.lang.IllegalArgumentException";
                *err_msg = AUSSOM_STRDUP("Illegal group reference: group index is missing");
                return false;
            }
            int64_t ref;
            if (repl[cur] == '{') {
                cur++;
                size_t ns = cur;
                while (cur < rl && ascii_alnum(repl[cur])) cur++;
                size_t nl = cur - ns;
                *err_class = "java.lang.IllegalArgumentException";
                if (nl == 0) {
                    *err_msg = AUSSOM_STRDUP("named capturing group has 0 length name");
                    return false;
                }
                if (cur >= rl || repl[cur] != '}') {
                    *err_msg = AUSSOM_STRDUP("named capturing group is missing trailing '}'");
                    return false;
                }
                /* The name is ASCII letters and digits. */
                char *name = AUSSOM_ALLOC_ATOMIC(nl + 1);
                uint32_t *uname = AUSSOM_ALLOC_ATOMIC((nl + 1) * sizeof(uint32_t));
                for (size_t k = 0; k < nl; k++) { name[k] = (char)repl[ns + k]; uname[k] = repl[ns + k]; }
                name[nl] = '\0';
                uname[nl] = 0;
                size_t mn = nl + 64;
                *err_msg = AUSSOM_ALLOC_ATOMIC(mn);
                if (name[0] >= '0' && name[0] <= '9') {
                    snprintf(*err_msg, mn, "capturing group name {%s} starts with digit character", name);
                    return false;
                }
                int num = pcre2_substring_number_from_name(code, (PCRE2_SPTR)uname);
                if (num < 0) {
                    snprintf(*err_msg, mn, "No group with name {%s}", name);
                    return false;
                }
                ref = num;
                cur++;
            } else {
                ref = (int64_t)repl[cur] - '0';
                if (ref < 0 || ref > 9) {
                    *err_class = "java.lang.IllegalArgumentException";
                    *err_msg = AUSSOM_STRDUP("Illegal group reference");
                    return false;
                }
                cur++;
                /* The longest group number that exists. */
                while (cur < rl && repl[cur] >= '0' && repl[cur] <= '9') {
                    int64_t next = ref * 10 + (repl[cur] - '0');
                    if ((int64_t)groups < next) break;
                    ref = next;
                    cur++;
                }
            }
            if (ref > (int64_t)groups) {
                *err_class = "java.lang.IndexOutOfBoundsException";
                *err_msg = AUSSOM_ALLOC_ATOMIC(48);
                snprintf(*err_msg, 48, "No group %lld", (long long)ref);
                return false;
            }
            if ((uint32_t)ref < ovn && ov[2 * ref] != PCRE2_UNSET) {
                size_t gs = rs->at[ov[2 * ref]], ge = rs->at[ov[2 * ref + 1]];
                sub_put(out, rs->u + gs, ge - gs);
            }
        } else {
            sub_put(out, repl + cur, 1);
            cur++;
        }
    }
    return true;
}

/* Shared substitute used by both the regex class and the string regex
 * methods. `global` selects replaceAll vs replaceFirst. It follows
 * Matcher.replaceAll / replaceFirst: the replacement string uses Java's
 * syntax, and after an empty match the next search starts one
 * character on.
 *
 * A bad replacement string is an exception the two callers report
 * differently, as Java's do: AussomString lets it escape (a thrown
 * exception) and ARegex returns its message. So on that error the
 * returned exception's text is the bare message, and *repl_err_class is
 * set to the Java exception class; otherwise it is left NULL. */
av_type *aussom_regex_substitute(environment *env, const char *re,
                                 const char *repl, const char *hay, bool global,
                                 const char **repl_err_class)
{
    if (repl_err_class != NULL) *repl_err_class = NULL;
    av_type *err;
    pcre2_code *code = compile(re, &err);
    if (code == NULL) return err;

    rctx rc_;
    rctx_open(&rc_, env);

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    rsubj rs;
    rs_init(&rs, hay);
    size_t rl;
    const uint16_t *urepl = av_text_to_units(repl, strlen(repl), &rl);
    size_t start = 0;       /* where the next search begins */
    size_t copied = 0;      /* subject copied to the output so far */
    sub_buf out = { NULL, 0, 0 };
    sub_put(&out, NULL, 0);
    av_type *fail = NULL;

    while (start <= rs.nu) {
        av_type *stop = eval_check_control_env(env);
        if (stop != NULL) { fail = stop; break; }

        size_t ms, me;
        int rc = rs_match(code, &rs, start, 0, md, rc_.ctx, &ms, &me);
        fail = rctx_error(&rc_, re, rc);
        if (fail != NULL) break;
        if (rc < 0) break;
        sub_put(&out, rs.u + copied, ms - copied);
        const char *ecls = NULL;
        char *emsg = NULL;
        if (!expand_replacement(code, md, &rs, urepl, rl, &out, &ecls, &emsg)) {
            if (repl_err_class != NULL) *repl_err_class = ecls;
            fail = &av_exception_new(emsg)->base.base;
            break;
        }
        copied = me;
        if (!global) break;
        if (me == ms) {
            /* Step past an empty match by one unit, as Java does, even
             * into the middle of a surrogate pair. */
            if (me >= rs.nu) break;
            start = me + 1;
        } else {
            start = me;
        }
    }

    pcre2_match_data_free(md);
    rctx_close(&rc_);
    pcre2_code_free(code);
    if (fail != NULL) return fail;
    sub_put(&out, rs.u + copied, rs.nu - copied);
    return units_str(out.p, out.len);
}

/* Full-string (anchored) match, mirroring Java String.matches. */
av_type *aussom_regex_matches(environment *env, const char *re, const char *hay)
{
    av_type *err;
    pcre2_code *code = compile(re, &err);
    if (code == NULL) return err;

    rctx rc_;
    rctx_open(&rc_, env);

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    rsubj rs;
    rs_init(&rs, hay);
    size_t ms, me;
    /* The whole subject must match, as Matcher.matches requires. */
    int rc = rs_match(code, &rs, 0, PCRE2_ANCHORED | PCRE2_ENDANCHORED, md, rc_.ctx, &ms, &me);
    bool matched = rc >= 0;
    pcre2_match_data_free(md);
    av_type *fail = rctx_error(&rc_, re, rc);
    rctx_close(&rc_);
    pcre2_code_free(code);
    if (fail != NULL) return fail;
    return &av_bool_new(matched)->base.base;
}

/* Splits `hay` on `re`, returning the pieces. Mirrors Java's
 * Pattern.split, which is where String.split's behavior is actually
 * specified: trailing empty strings are dropped, leading and interior
 * ones are kept, and a zero-width match advances one character so the
 * scan terminates. */
av_type *aussom_regex_split(environment *env, const char *re, const char *hay,
                            av_list **out_parts)
{
    *out_parts = NULL;
    av_type *err;
    pcre2_code *code = compile(re, &err);
    if (code == NULL) return err;

    rctx rc_;
    rctx_open(&rc_, env);

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    rsubj rs;
    rs_init(&rs, hay);
    size_t piece_start = 0;   /* start of the piece being accumulated */
    size_t scan = 0;          /* where the next match attempt begins */
    bool found = false;
    av_list *parts = av_list_new();
    av_type *stop_err = NULL;

    while (scan <= rs.nu) {
        av_type *stop = eval_check_control_env(env);
        if (stop != NULL) { stop_err = stop; break; }

        size_t ms, me;
        int rc = rs_match(code, &rs, scan, 0, md, rc_.ctx, &ms, &me);
        stop_err = rctx_error(&rc_, re, rc);
        if (stop_err != NULL) break;
        if (rc < 0) break;   /* no further delimiter */
        found = true;
        scan = (me == ms) ? me + 1 : me;

        /* A zero-width match at the very start adds no leading empty
         * piece (Pattern.split since Java 8). */
        if (piece_start == 0 && ms == 0 && me == 0) continue;

        av_list_add(parts, units_str(rs.u + piece_start, ms - piece_start));
        piece_start = me;
    }

    pcre2_match_data_free(md);
    rctx_close(&rc_);
    pcre2_code_free(code);
    if (stop_err != NULL) return stop_err;

    /* No match at all: the input is the only piece, even when empty. */
    if (!found) {
        av_list_add(parts, units_str(rs.u, rs.nu));
        *out_parts = parts;
        return NULL;
    }

    /* The remainder after the last delimiter. */
    av_list_add(parts, units_str(rs.u + piece_start, rs.nu - piece_start));

    /* Pattern.split drops trailing empty strings. */
    while (av_list_size(parts) > 0) {
        av_type *last = av_list_get(parts, av_list_size(parts) - 1);
        if (last->type != C_TYPE_STRING) break;
        if (av_string_length((av_string *)last) != 0) break;
        av_list_remove_at(parts, av_list_size(parts) - 1);
    }

    *out_parts = parts;
    return NULL;
}

static av_type *do_substitute(environment *env, av_list *args, bool global)
{
    av_type *aerr = NULL;
    const char *re   = req_str_arg(args, 0, &aerr); if (aerr) return aerr;
    const char *repl = req_str_arg(args, 1, &aerr); if (aerr) return aerr;
    const char *hay  = req_str_arg(args, 2, &aerr); if (aerr) return aerr;
    /* ARegex returns a bad replacement's message as its exception. */
    return aussom_regex_substitute(env, re, repl, hay, global, NULL);
}

static av_type *regex_replace(av_type *self, environment *env, av_list *args)
{ (void)self; return do_substitute(env, args, true); }

static av_type *regex_replaceFirst(av_type *self, environment *env, av_list *args)
{ (void)self; return do_substitute(env, args, false); }

const aussom_extern_method stdlib_aregex_methods[] = {
    { "match", regex_match },
    { "replace", regex_replace },
    { "replaceFirst", regex_replaceFirst },
    { NULL, NULL }
};
