/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Shared JSON serializer for av_to_json (the toJson extern on every
 * type). Mirrors the per-type toJson methods in the Java runtime,
 * unified into one recursive walk.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/memstream.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/ast_class.h"
#include "aussom/av_object.h"
#include "aussom/extern_registry.h"
#include "aussom/stack_guard.h"
#include "aussom/stdlib_packages.h"
#include "stdlib_internal.h"

/* Writes `s` escaped as json-simple's JSONValue.escape does it, without
 * the surrounding quotes: the usual escapes, '/' as \/, and the control
 * ranges U+0000-U+001F and U+007F-U+009F plus U+2000-U+20FF as \uXXXX
 * in upper-case hex. AussomString.toJson and json.escape use it. */
void av_json_escape(FILE *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        switch (c) {
        case '"':  fputs("\\\"", out); p++; continue;
        case '\\': fputs("\\\\", out); p++; continue;
        case '\n': fputs("\\n", out); p++; continue;
        case '\r': fputs("\\r", out); p++; continue;
        case '\t': fputs("\\t", out); p++; continue;
        case '\b': fputs("\\b", out); p++; continue;
        case '\f': fputs("\\f", out); p++; continue;
        case '/':  fputs("\\/", out); p++; continue;
        default: break;
        }
        if (c < 0x20 || c == 0x7F) { fprintf(out, "\\u%04X", c); p++; continue; }
        if (c < 0x80) { fputc(c, out); p++; continue; }
        /* Decode one UTF-8 sequence to test the escaped ranges. */
        uint32_t cp = 0;
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { fputc(c, out); p++; continue; }
        size_t k = 1;
        for (; k < len && (p[k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (p[k] & 0x3F);
        if (k < len) { fputc(c, out); p++; continue; }
        /* cp is 0 for C0 80, the stored form of U+0000. */
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || (cp >= 0x2000 && cp <= 0x20FF))
            fprintf(out, "\\u%04X", (unsigned)cp);
        else
            fwrite(p, 1, len, out);
        p += len;
    }
}

/* A toJson string value, a pack string and a map key in either form:
 * quoted and escaped as above (JSONValue.escape). */
static void json_string(FILE *out, const char *s)
{
    fputc('"', out);
    av_json_escape(out, s);
    fputc('"', out);
}

/* An object member key in either form, and the Date and Buffer pack
 * values, with the standard escapes only. (Java writes these with no
 * escaping at all.) */
static void pack_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
        default:
            if (*p < 0x20) fprintf(out, "\\u%04x", *p);
            else if (p[0] == 0xC0 && p[1] == 0x80) { fputs("\\u0000", out); p++; }
            else fputc(*p, out);
        }
    }
    fputc('"', out);
}

/* Returns false (and writes nothing useful) when the subtree holds a
 * type with no JSON representation, or when the native stack ran low
 * (the thread is then tripped; see stack_guard.h). Containers are read
 * through snapshots so no lock is held while recursing: a container
 * that holds itself recurses until the stack guard stops it, rather
 * than waiting on its own lock. */
static bool json_write(av_type *v, FILE *out)
{
    if (stack_guard_low()) return false;
    switch (v->type) {
    case C_TYPE_NULL:   fputs("null", out); return true;
    case C_TYPE_BOOL:   fputs(av_bool_get((av_bool *)v) ? "true" : "false", out); return true;
    case C_TYPE_INT:    fprintf(out, "%" PRId64, av_int_get((av_int *)v)); return true;
    /* AussomDouble.toJson is its str(): Java's Double.toString (7.0). */
    case C_TYPE_DOUBLE: fputs(av_str(v), out); return true;
    case C_TYPE_STRING: json_string(out, av_string_get((av_string *)v)); return true;
    case C_TYPE_LIST: {
        size_t n;
        av_type **items = av_vec_snapshot(av_list_vec((av_list *)v), &n);
        fputc('[', out);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) fputc(',', out);
            if (!json_write(items[i], out)) return false;
        }
        fputc(']', out);
        return true;
    }
    case C_TYPE_MAP: {
        const char **keys;
        av_type **vals;
        size_t n = av_strmap_snapshot(av_map_strmap((av_map *)v), &keys, &vals);
        fputc('{', out);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) fputc(',', out);
            json_string(out, keys[i]);
            fputc(':', out);
            if (!json_write(vals[i], out)) return false;
        }
        fputc('}', out);
        return true;
    }
    case C_TYPE_OBJECT: {
        av_object *o = (av_object *)v;
        /* AussomObject.toJson: an extern object that has a JSON form of
         * its own writes it (a value, ABuffer, ADate). */
        av_type *ev = av_object_get_extern_value(o);
        if (ev != NULL) return json_write(ev, out);
        ast_class *cd = (ast_class *)av_object_get_class_def(o);
        if (cd != NULL && ast_class_instance_of(cd, "Date")) {
            /* ADate.toJson is its bare ISO string, unquoted. */
            const char *iso = av_date_iso_str(v);
            if (iso != NULL) { fputs(iso, out); return true; }
        }
        if (cd != NULL && ast_class_instance_of(cd, "Buffer")) {
            const char *enc = av_buffer_encoded_str(v);
            if (enc != NULL) { fprintf(out, "\"%s\"", enc); return true; }
        }
        fputc('{', out);
        if (o->members != NULL) {
            const char **keys;
            av_type **vals;
            size_t n = av_strmap_snapshot(o->members, &keys, &vals);
            for (size_t i = 0; i < n; i++) {
                if (i > 0) fputc(',', out);
                pack_string(out, keys[i]);
                fputc(':', out);
                if (!json_write(vals[i], out)) return false;
            }
        }
        fputc('}', out);
        return true;
    }
    default:
        return false;
    }
}

av_type *av_to_json(av_type *v)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) return &av_exception_new("toJson: out of memory.")->base.base;
    bool ok = json_write(v, fp);
    aussom_memstream_close(fp, &buf, &len);
    if (!ok) {
        aussom_memstream_free(buf);
        /* An exception or a callback has no JSON form: AussomObject.toJson
         * reaches getClassDef(), which is null for them, and throws. */
        return &av_exception_new_thrown("java.lang.NullPointerException: Cannot invoke "
            "\"com.aussom.ast.astClass.getExternClass()\" because the return value of "
            "\"com.aussom.types.AussomObject.getClassDef()\" is null")->base.base;
    }
    char *out = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return &av_string_new(out)->base.base;
}

/* --- pack (object.pack and friends) ------------------------------ */

/* Writes the Aussom pack form of `v`. Mirrors the per-type pack() in
 * the Java runtime. Returns false on a non-packable type. Note the
 * Java quirk reproduced here: a null's type tag is its class name
 * "cnull" (unpack accepts both that and "null"). */
static bool pack_write(av_type *v, FILE *out)
{
    /* Same stack guard and snapshots as json_write. */
    if (stack_guard_low()) return false;
    switch (v->type) {
    case C_TYPE_NULL:
        fputs("{\"type\":\"cnull\",\"value\":null}", out);
        return true;
    case C_TYPE_BOOL:
        fprintf(out, "{\"type\":\"bool\",\"value\":%s}",
                av_bool_get((av_bool *)v) ? "true" : "false");
        return true;
    case C_TYPE_INT:
        fprintf(out, "{\"type\":\"int\",\"value\":%" PRId64 "}", av_int_get((av_int *)v));
        return true;
    case C_TYPE_DOUBLE:
        /* str() form (7.0), matching Java's Double.toString, so the
         * packed value round-trips back to a double. */
        fprintf(out, "{\"type\":\"double\",\"value\":%s}", av_str(v));
        return true;
    case C_TYPE_STRING:
        fputs("{\"type\":\"string\",\"value\":", out);
        json_string(out, av_string_get((av_string *)v));
        fputc('}', out);
        return true;
    case C_TYPE_LIST: {
        /* Elements are PACKED, not toJson'd, so a packed list round
         * trips through unpack. (Through 1.2.x these were written with
         * toJson, which dropped the per-element type tags and made the
         * result un-unpackable; since fixed in aussom-base.) */
        size_t n;
        av_type **items = av_vec_snapshot(av_list_vec((av_list *)v), &n);
        fputs("{\"type\":\"list\",\"value\":[", out);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) fputc(',', out);
            if (!pack_write(items[i], out)) return false;
        }
        fputs("]}", out);
        return true;
    }
    case C_TYPE_MAP: {
        const char **keys;
        av_type **vals;
        size_t n = av_strmap_snapshot(av_map_strmap((av_map *)v), &keys, &vals);
        fputs("{\"type\":\"map\",\"value\":{", out);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) fputc(',', out);
            json_string(out, keys[i]);
            fputc(':', out);
            if (!pack_write(vals[i], out)) return false;
        }
        fputs("}}", out);
        return true;
    }
    case C_TYPE_OBJECT: {
        av_object *o = (av_object *)v;
        av_type *ev = av_object_get_extern_value(o);
        if (ev != NULL) return pack_write(ev, out);
        ast_class *cd = (ast_class *)av_object_get_class_def(o);
        const char *name = (cd != NULL) ? cd->base.name : "object";
        if (cd != NULL && strcmp(name, "Date") == 0) {
            const char *iso = av_date_iso_str(v);
            if (iso == NULL) return false;
            fputs("{\"type\":\"Date\",\"value\":", out);
            pack_string(out, iso);
            fputc('}', out);
            return true;
        }
        if (cd != NULL && strcmp(name, "Buffer") == 0) {
            const char *enc = av_buffer_encoded_str(v);
            /* NULL means the object is named "Buffer" but is not a
             * genuine one; fall through to generic output
             * rather than read its header as a buf_state. */
            if (enc != NULL) {
                fputs("{\"type\":\"Buffer\",\"value\":", out);
                pack_string(out, enc);
                fputc('}', out);
                return true;
            }
        }
        fprintf(out, "{\"type\":\"%s\",\"members\":{", name);
        if (o->members != NULL) {
            const char **keys;
            av_type **vals;
            size_t n = av_strmap_snapshot(o->members, &keys, &vals);
            for (size_t i = 0; i < n; i++) {
                if (i > 0) fputc(',', out);
                pack_string(out, keys[i]);
                fputc(':', out);
                if (!pack_write(vals[i], out)) return false;
            }
        }
        fputs("}}", out);
        return true;
    }
    default:
        return false;
    }
}

av_type *av_pack(av_type *v)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) return &av_exception_new("pack: out of memory.")->base.base;
    bool ok = pack_write(v, fp);
    aussom_memstream_close(fp, &buf, &len);
    if (!ok) {
        aussom_memstream_free(buf);
        /* The same for pack: AussomObject.pack reaches getClassDef(). */
        return &av_exception_new_thrown("java.lang.NullPointerException: Cannot invoke "
            "\"com.aussom.ast.astClass.getExtern()\" because the return value of "
            "\"com.aussom.types.AussomObject.getClassDef()\" is null")->base.base;
    }
    char *out = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return &av_string_new(out)->base.base;
}

av_type *stdlib_pack(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    return av_pack(self);
}

/* ADate.str(int) and ABuffer.str(int), for av_object_write_str. */
static bool native_str(const av_object *o, int level, FILE *out)
{
    (void)level;
    ast_class *cd = (ast_class *)av_object_get_class_def(o);
    if (cd == NULL) return false;
    av_type *v = (av_type *)&o->base;
    if (ast_class_instance_of(cd, "Date")) {
        const char *iso = av_date_iso_str(v);
        if (iso == NULL) return false;
        fprintf(out, "\"%s\"", iso);
        return true;
    }
    if (ast_class_instance_of(cd, "Buffer")) {
        void *eo = av_object_get_extern_object(o);
        if (eo == NULL || eo == (void *)o || ((buf_state *)eo)->magic != BUF_MAGIC)
            return false;
        fprintf(out, "Buffer(%d)", ((buf_state *)eo)->size);
        return true;
    }
    return false;
}

void stdlib_install_native_str(void)
{
    av_object_set_native_str(native_str);
}

/* AussomNull.isBlank: null always counts as blank. */
static av_type *null_isBlank(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env; (void)args;
    return &av_bool_new(true)->base.base;
}

const aussom_extern_method stdlib_aussom_null_methods[] = {
    { "pack", stdlib_pack },
    { "isBlank", null_isBlank },
    { NULL, NULL }
};
