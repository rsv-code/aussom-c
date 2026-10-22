/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * A port of json-simple 1.1.1 (org.json.simple.parser): the JFlex
 * lexer Yylex and the JSONParser state machine. See json_simple.h.
 *
 * Positions in error text count UTF-16 code units, as Java's do.
 */

#include "aussom/json_simple.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av_double.h"
#include "aussom/av_string.h"

/* --- values ------------------------------------------------------ */

static js_value *js_new(js_kind k)
{
    js_value *v = AUSSOM_ALLOC(sizeof(js_value));
    memset(v, 0, sizeof(*v));
    v->kind = k;
    return v;
}

static void js_grow(js_value *c)
{
    if (c->count < c->cap) return;
    size_t cap = c->cap ? c->cap * 2 : 8;
    js_value **items = AUSSOM_ALLOC(cap * sizeof(*items));
    if (c->count) memcpy(items, c->items, c->count * sizeof(*items));
    c->items = items;
    if (c->kind == JS_OBJECT) {
        char **keys = AUSSOM_ALLOC(cap * sizeof(*keys));
        if (c->count) memcpy(keys, c->keys, c->count * sizeof(*keys));
        c->keys = keys;
    }
    c->cap = cap;
}

static void js_add(js_value *arr, js_value *v)
{
    js_grow(arr);
    arr->items[arr->count++] = v;
}

/* JSONObject is a HashMap: a repeated key replaces the earlier value. */
static void js_put(js_value *obj, char *key, js_value *v)
{
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) { obj->items[i] = v; return; }
    }
    js_grow(obj);
    obj->keys[obj->count] = key;
    obj->items[obj->count++] = v;
}

js_value *js_get(const js_value *obj, const char *key)
{
    if (obj == NULL || obj->kind != JS_OBJECT) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

const char *js_java_class(const js_value *v)
{
    switch (v->kind) {
    case JS_STRING: return "java.lang.String";
    case JS_LONG:   return "java.lang.Long";
    case JS_DOUBLE: return "java.lang.Double";
    case JS_BOOL:   return "java.lang.Boolean";
    case JS_OBJECT: return "org.json.simple.JSONObject";
    case JS_ARRAY:  return "org.json.simple.JSONArray";
    default:        return "null";
    }
}

/* --- string buffer ----------------------------------------------- */

typedef struct sbuf { char *p; size_t len, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 32;
        while (b->len + n + 1 > cap) cap *= 2;
        char *np = AUSSOM_ALLOC_ATOMIC(cap);
        if (b->len) memcpy(np, b->p, b->len);
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_cp(sbuf *b, uint32_t cp)
{
    char u[4];
    size_t n;
    /* U+0000 and surrogates use the lossless char form (av_string.h). */
    if (cp == 0) { u[0] = (char)0xC0; u[1] = (char)0x80; n = 2; }
    else if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    sb_put(b, u, n);
}

/* --- lexer (Yylex) ----------------------------------------------- */

typedef enum {
    T_VALUE, T_LEFT_BRACE, T_RIGHT_BRACE, T_LEFT_SQUARE, T_RIGHT_SQUARE,
    T_COMMA, T_COLON, T_EOF
} tok_type;

typedef struct lexer {
    const unsigned char *s;
    size_t i;        /* byte offset */
    long pos;        /* UTF-16 offset of i (yychar) */
    const char *err; /* set when a token could not be read */
} lexer;

/* UTF-16 units in the UTF-8 sequence at p. */
static int u16_units(const unsigned char *p, size_t *bytes)
{
    unsigned char c = *p;
    if (c < 0x80) { *bytes = 1; return 1; }
    if ((c & 0xE0) == 0xC0) { *bytes = 2; return 1; }
    if ((c & 0xF0) == 0xE0) { *bytes = 3; return 1; }
    if ((c & 0xF8) == 0xF0) { *bytes = 4; return 2; }
    *bytes = 1;
    return 1;
}

static void advance(lexer *lx, size_t bytes_to)
{
    while (lx->i < bytes_to) {
        size_t b;
        lx->pos += u16_units(lx->s + lx->i, &b);
        lx->i += b;
    }
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int hexv(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *fmt_err(const char *fmt, const char *a, long pos)
{
    size_t n = strlen(fmt) + (a ? strlen(a) : 0) + 32;
    char *m = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(m, n, fmt, a, pos);
    return m;
}

/* The finished string, re-encoded through UTF-16 units so a surrogate
 * pair written as two \u escapes becomes one character. */
static char *string_value(const sbuf *b)
{
    size_t n;
    uint16_t *u = av_text_to_units(b->p, b->len, &n);
    return av_units_to_text(u, n, NULL);
}

/* The STRING_BEGIN state: everything after the opening quote. A \u
 * escape keeps U+0000 and unpaired surrogates, as a Java string does. */
static bool lex_string(lexer *lx, js_value **out)
{
    sbuf b = { NULL, 0, 0 };
    sb_put(&b, "", 0);
    for (;;) {
        unsigned char c = lx->s[lx->i];
        if (c == '\0') {
            /* EOF inside the string: Yylex returns end of file. */
            *out = NULL;
            return true;
        }
        if (c == '"') {
            advance(lx, lx->i + 1);
            js_value *v = js_new(JS_STRING);
            v->str = string_value(&b);
            *out = v;
            return true;
        }
        if (c != '\\') {
            size_t start = lx->i, end = lx->i;
            while (lx->s[end] != '\0' && lx->s[end] != '"' && lx->s[end] != '\\') end++;
            sb_put(&b, (const char *)lx->s + start, end - start);
            advance(lx, end);
            continue;
        }
        unsigned char e = lx->s[lx->i + 1];
        const char *rep = NULL;
        switch (e) {
        case '"': rep = "\""; break;
        case '\\': rep = "\\"; break;
        case '/': rep = "/"; break;
        case 'b': rep = "\b"; break;
        case 'f': rep = "\f"; break;
        case 'n': rep = "\n"; break;
        case 'r': rep = "\r"; break;
        case 't': rep = "\t"; break;
        default: break;
        }
        if (rep != NULL) {
            sb_put(&b, rep, 1);
            advance(lx, lx->i + 2);
            continue;
        }
        if (e == 'u' && hexv(lx->s[lx->i + 2]) >= 0 && hexv(lx->s[lx->i + 3]) >= 0
            && hexv(lx->s[lx->i + 4]) >= 0 && hexv(lx->s[lx->i + 5]) >= 0) {
            uint32_t u = (uint32_t)((hexv(lx->s[lx->i + 2]) << 12) | (hexv(lx->s[lx->i + 3]) << 8)
                                    | (hexv(lx->s[lx->i + 4]) << 4) | hexv(lx->s[lx->i + 5]));
            advance(lx, lx->i + 6);
            sb_cp(&b, u);
            continue;
        }
        /* A backslash no rule matches: JFlex's scan error. */
        lx->err = "java.lang.Error: Error: could not match input";
        return false;
    }
}

/* Reads the next token. Returns false with lx->err set on a lexer
 * error. *pos is set to the token's start (yychar). */
static bool next_token(lexer *lx, tok_type *t, js_value **val, long *pos)
{
    *val = NULL;
    for (;;) {
        unsigned char c = lx->s[lx->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { advance(lx, lx->i + 1); continue; }
        *pos = lx->pos;
        if (c == '\0') { *t = T_EOF; return true; }
        switch (c) {
        case '{': advance(lx, lx->i + 1); *t = T_LEFT_BRACE; return true;
        case '}': advance(lx, lx->i + 1); *t = T_RIGHT_BRACE; return true;
        case '[': advance(lx, lx->i + 1); *t = T_LEFT_SQUARE; return true;
        case ']': advance(lx, lx->i + 1); *t = T_RIGHT_SQUARE; return true;
        case ',': advance(lx, lx->i + 1); *t = T_COMMA; return true;
        case ':': advance(lx, lx->i + 1); *t = T_COLON; return true;
        case '"': {
            advance(lx, lx->i + 1);
            js_value *sv;
            if (!lex_string(lx, &sv)) return false;
            if (sv == NULL) { *pos = lx->pos; *t = T_EOF; return true; }
            *val = sv;
            *t = T_VALUE;
            return true;
        }
        default: break;
        }
        const char *rest = (const char *)lx->s + lx->i;
        if (strncmp(rest, "true", 4) == 0 || strncmp(rest, "false", 5) == 0) {
            bool tv = rest[0] == 't';
            advance(lx, lx->i + (tv ? 4 : 5));
            js_value *v = js_new(JS_BOOL);
            v->bval = tv;
            *val = v;
            *t = T_VALUE;
            return true;
        }
        if (strncmp(rest, "null", 4) == 0) {
            advance(lx, lx->i + 4);
            *val = js_new(JS_NULL);
            *t = T_VALUE;
            return true;
        }
        /* INT = -?[0-9]+ ; DOUBLE = INT (\.[0-9]+)? ([eE][-+]?[0-9]+)? */
        size_t j = lx->i;
        if (lx->s[j] == '-') j++;
        if (is_digit(lx->s[j])) {
            while (is_digit(lx->s[j])) j++;
            bool dbl = false;
            if (lx->s[j] == '.' && is_digit(lx->s[j + 1])) {
                j++;
                while (is_digit(lx->s[j])) j++;
                dbl = true;
            }
            if ((lx->s[j] == 'e' || lx->s[j] == 'E')) {
                size_t k = j + 1;
                if (lx->s[k] == '+' || lx->s[k] == '-') k++;
                if (is_digit(lx->s[k])) {
                    while (is_digit(lx->s[k])) k++;
                    j = k;
                    dbl = true;
                }
            }
            size_t n = j - lx->i;
            char *txt = AUSSOM_ALLOC_ATOMIC(n + 1);
            memcpy(txt, rest, n);
            txt[n] = '\0';
            js_value *v;
            if (dbl) {
                v = js_new(JS_DOUBLE);
                v->dval = strtod(txt, NULL);
            } else {
                errno = 0;
                long long lv = strtoll(txt, NULL, 10);
                if (errno == ERANGE) {
                    /* Long.valueOf throws for a value past a long. */
                    size_t mn = n + 64;
                    char *m = AUSSOM_ALLOC_ATOMIC(mn);
                    snprintf(m, mn, "java.lang.NumberFormatException: For input string: \"%s\"", txt);
                    lx->err = m;
                    return false;
                }
                v = js_new(JS_LONG);
                v->lval = (int64_t)lv;
            }
            advance(lx, j);
            *val = v;
            *t = T_VALUE;
            return true;
        }
        /* Any other character: ParseException ERROR_UNEXPECTED_CHAR. */
        size_t bytes;
        u16_units(lx->s + lx->i, &bytes);
        char ch[5];
        memcpy(ch, rest, bytes);
        ch[bytes] = '\0';
        lx->err = fmt_err("Unexpected character (%s) at position %ld.", ch, lx->pos);
        return false;
    }
}

/* Yytoken.toString. */
static const char *tok_str(tok_type t, const js_value *v)
{
    switch (t) {
    case T_LEFT_BRACE:   return "LEFT BRACE({)";
    case T_RIGHT_BRACE:  return "RIGHT BRACE(})";
    case T_LEFT_SQUARE:  return "LEFT SQUARE([)";
    case T_RIGHT_SQUARE: return "RIGHT SQUARE(])";
    case T_COMMA:        return "COMMA(,)";
    case T_COLON:        return "COLON(:)";
    case T_EOF:          return "END OF FILE";
    default: break;
    }
    /* VALUE(<value's toString>). */
    char buf[64];
    const char *s;
    switch (v->kind) {
    case JS_STRING: s = v->str; break;
    case JS_LONG:   snprintf(buf, sizeof(buf), "%lld", (long long)v->lval); s = buf; break;
    case JS_DOUBLE: av_double_java_str(v->dval, buf, sizeof(buf)); s = buf; break;
    case JS_BOOL:   s = v->bval ? "true" : "false"; break;
    default:        s = "null"; break;
    }
    size_t n = strlen(s) + 8;
    char *m = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(m, n, "VALUE(%s)", s);
    return m;
}

/* --- parser (JSONParser.parse) ----------------------------------- */

enum { S_INIT, S_IN_FINISHED_VALUE, S_IN_OBJECT, S_IN_ARRAY, S_PASSED_PAIR_KEY, S_IN_ERROR = -1 };

typedef struct stack { void **p; size_t n, cap; } stack;

static void push(stack *s, void *v)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        void **np = AUSSOM_ALLOC(cap * sizeof(void *));
        if (s->n) memcpy(np, s->p, s->n * sizeof(void *));
        s->p = np;
        s->cap = cap;
    }
    s->p[s->n++] = v;
}
static void *pop(stack *s) { return s->p[--s->n]; }
static void *top(stack *s) { return s->p[s->n - 1]; }

#define ST(x) ((void *)(intptr_t)(x))
#define STV(p) ((int)(intptr_t)(p))

static const char *unexpected_token(tok_type t, const js_value *v, long pos)
{
    return fmt_err("Unexpected token %s at position %ld.", tok_str(t, v), pos);
}

js_value *js_parse(const char *text, const char **err)
{
    lexer lx = { (const unsigned char *)text, 0, 0, NULL };
    stack status = { NULL, 0, 0 }, values = { NULL, 0, 0 };
    int st = S_INIT;
    tok_type t;
    js_value *v;
    long pos;
    *err = NULL;
    do {
        if (!next_token(&lx, &t, &v, &pos)) { *err = lx.err; return NULL; }
        switch (st) {
        case S_INIT:
            if (t == T_VALUE) { st = S_IN_FINISHED_VALUE; push(&status, ST(st)); push(&values, v); }
            else if (t == T_LEFT_BRACE) { st = S_IN_OBJECT; push(&status, ST(st)); push(&values, js_new(JS_OBJECT)); }
            else if (t == T_LEFT_SQUARE) { st = S_IN_ARRAY; push(&status, ST(st)); push(&values, js_new(JS_ARRAY)); }
            else st = S_IN_ERROR;
            break;
        case S_IN_FINISHED_VALUE:
            if (t == T_EOF) return (js_value *)pop(&values);
            *err = unexpected_token(t, v, pos);
            return NULL;
        case S_IN_OBJECT:
            if (t == T_COMMA) break;
            if (t == T_VALUE) {
                if (v->kind == JS_STRING) { push(&values, v); st = S_PASSED_PAIR_KEY; push(&status, ST(st)); }
                else st = S_IN_ERROR;
            } else if (t == T_RIGHT_BRACE) {
                if (values.n > 1) { pop(&status); pop(&values); st = status.n ? STV(top(&status)) : -1; }
                else st = S_IN_FINISHED_VALUE;
            } else st = S_IN_ERROR;
            break;
        case S_PASSED_PAIR_KEY:
            if (t == T_COLON) break;
            if (t == T_VALUE) {
                pop(&status);
                js_value *key = (js_value *)pop(&values);
                js_put((js_value *)top(&values), key->str, v);
                st = STV(top(&status));
            } else if (t == T_LEFT_SQUARE || t == T_LEFT_BRACE) {
                pop(&status);
                js_value *key = (js_value *)pop(&values);
                js_value *nc = js_new(t == T_LEFT_SQUARE ? JS_ARRAY : JS_OBJECT);
                js_put((js_value *)top(&values), key->str, nc);
                st = (t == T_LEFT_SQUARE) ? S_IN_ARRAY : S_IN_OBJECT;
                push(&status, ST(st));
                push(&values, nc);
            } else st = S_IN_ERROR;
            break;
        case S_IN_ARRAY:
            if (t == T_COMMA) break;
            if (t == T_VALUE) js_add((js_value *)top(&values), v);
            else if (t == T_RIGHT_SQUARE) {
                if (values.n > 1) { pop(&status); pop(&values); st = status.n ? STV(top(&status)) : -1; }
                else st = S_IN_FINISHED_VALUE;
            } else if (t == T_LEFT_BRACE || t == T_LEFT_SQUARE) {
                js_value *nc = js_new(t == T_LEFT_BRACE ? JS_OBJECT : JS_ARRAY);
                js_add((js_value *)top(&values), nc);
                st = (t == T_LEFT_BRACE) ? S_IN_OBJECT : S_IN_ARRAY;
                push(&status, ST(st));
                push(&values, nc);
            } else st = S_IN_ERROR;
            break;
        default:
            *err = unexpected_token(t, v, pos);
            return NULL;
        }
        if (st == S_IN_ERROR) { *err = unexpected_token(t, v, pos); return NULL; }
    } while (t != T_EOF);
    *err = unexpected_token(t, v, pos);
    return NULL;
}
