/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `json` class (com.aussom.stdlib.AJson): parse, escape
 * and unpack. Parsing is json_simple.c, a port of the json-simple
 * parser Java uses, so the same text is accepted, refused and reported
 * the same way.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aussom/memstream.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/av_object.h"
#include "aussom/ast_eval.h"
#include "aussom/engine.h"
#include "aussom/extern_registry.h"
#include "aussom/stack_guard.h"
#include "aussom/json_simple.h"
#include "codec.h"
#include "stdlib_internal.h"

/* A thrown Java exception, for the call to report as
 * EXTERN_INVOCATION_TARGET_EXCEPTION. */
static av_type *thrown(const char *text)
{
    return &av_exception_new_thrown(text)->base.base;
}

/* An aussomException AJson throws. */
static av_type *thrown_msg(const char *msg)
{
    return &av_exception_new_thrown_aussom(msg)->base.base;
}

/* AJson.parseJsonDataType and its object and array helpers. Recurses
 * once per nesting level, so it checks the stack itself; when it runs
 * low the call reports STACK_OVERFLOW. */
static av_type *js_to_av(const js_value *v)
{
    if (stack_guard_low()) return &av_null_new()->base.base;
    switch (v->kind) {
    case JS_STRING: return &av_string_new(v->str)->base.base;
    case JS_LONG:   return &av_int_new(v->lval)->base.base;
    case JS_DOUBLE: return &av_double_new(v->dval)->base.base;
    case JS_BOOL:   return &av_bool_new(v->bval)->base.base;
    case JS_NULL:   return &av_null_new()->base.base;
    case JS_ARRAY: {
        av_list *l = av_list_new();
        for (size_t i = 0; i < v->count; i++) av_list_add(l, js_to_av(v->items[i]));
        return &l->base.base;
    }
    case JS_OBJECT: {
        av_map *m = av_map_new();
        for (size_t i = 0; i < v->count; i++) av_map_put(m, v->keys[i], js_to_av(v->items[i]));
        return &m->base.base;
    }
    }
    return &av_null_new()->base.base;
}

/* The ClassCastException of casting `v` to `target`. */
static av_type *cast_error(const js_value *v, const char *target)
{
    char m[192];
    snprintf(m, sizeof(m), "java.lang.ClassCastException: class %s cannot be cast to class %s",
             js_java_class(v), target);
    return thrown(m);
}

/* AJson.parse: an array becomes a list; anything else is cast to a
 * JSONObject, which fails for a bare value. */
static av_type *json_parse(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self;
    const char *jstr = av_list_get_string(args, 0);
    if (jstr == NULL) return &av_exception_new("json_parse: argument 1 must be a string.")->base.base;
    const char *err;
    js_value *root = js_parse(jstr, &err);
    if (root == NULL) return thrown(err);
    if (root->kind == JS_ARRAY || root->kind == JS_OBJECT) return js_to_av(root);
    if (root->kind == JS_NULL) {
        return thrown("java.lang.NullPointerException: Cannot invoke "
                      "\"org.json.simple.JSONObject.keySet()\" because \"jobj\" is null");
    }
    return cast_error(root, "org.json.simple.JSONObject");
}

/* Mirrors org.json.simple JSONObject.escape: escapes the standard
 * control/quote characters in the content (no surrounding quotes). */
static av_type *json_escape(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self;
    const char *s = av_list_get_string(args, 0);
    if (s == NULL) return &av_exception_new("json_escape: argument 1 must be a string.")->base.base;

    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) {
        return &av_exception_new("json.escape(): out of memory.")->base.base;
    }
    /* JSONObject.escape: json-simple's JSONValue.escape. */
    av_json_escape(fp, s);
    aussom_memstream_close(fp, &buf, &len);
    char *out = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return &av_string_new(out)->base.base;
}

/* --- unpack (inverse of object.pack) ----------------------------- */

static av_type *unpack_node(environment *env, const js_value *obj);

/* tobj.getClass() on a JSON null throws before any message is built. */
static av_type *null_class_error(void)
{
    return thrown("java.lang.NullPointerException: Cannot invoke "
                  "\"Object.getClass()\" because \"tobj\" is null");
}

/* AJson's "Getting value for key '<key>'. Expecting object of type
 * <want> but found '<class>' instead." */
static av_type *wrong_type(const char *key, const char *want, const js_value *v)
{
    if (v->kind == JS_NULL) return null_class_error();
    char m[256];
    snprintf(m, sizeof(m), "json.unpack(): Getting value for key '%s'. Expecting object "
             "of type %s but found '%s' instead.", key, want, js_java_class(v));
    return thrown_msg(m);
}

static av_type *missing_value(void)
{
    return thrown_msg("json.unpack(): Malformed JSON, missing value.");
}

/* AJson.unpackJsonData. Every error it raises is thrown. */
static av_type *unpack_node(environment *env, const js_value *obj)
{
    if (stack_guard_low()) return &av_null_new()->base.base;
    const js_value *ty = js_get(obj, "type");
    if (ty == NULL) return thrown_msg("json.unpack(): Malformed JSON, 'type' not found.");
    if (ty->kind != JS_STRING) return wrong_type("type", "String", ty);
    const char *type = ty->str;
    const js_value *val = js_get(obj, "value");

    if (strcmp(type, "bool") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_BOOL) return wrong_type("value", "Boolean", val);
        return &av_bool_new(val->bval)->base.base;
    }
    if (strcmp(type, "int") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_LONG) return wrong_type("value", "Long", val);
        return &av_int_new(val->lval)->base.base;
    }
    if (strcmp(type, "double") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_DOUBLE) return wrong_type("value", "Double", val);
        return &av_double_new(val->dval)->base.base;
    }
    if (strcmp(type, "string") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_STRING) return wrong_type("value", "String", val);
        return &av_string_new(val->str)->base.base;
    }
    if (strcmp(type, "null") == 0 || strcmp(type, "cnull") == 0) {
        if (val == NULL) return missing_value();
        return &av_null_new()->base.base;
    }
    if (strcmp(type, "list") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_ARRAY) return wrong_type("value", "JSONArray", val);
        av_list *l = av_list_new();
        for (size_t i = 0; i < val->count; i++) {
            const js_value *e = val->items[i];
            if (e->kind != JS_OBJECT) {
                char m[256];
                snprintf(m, sizeof(m), "json.unpack(): Malformed JSON structure found for "
                         "array 'value' at index %zu, expecting JSON object but found "
                         "'%s' instead.", i, js_java_class(e));
                return thrown_msg(m);
            }
            av_type *item = unpack_node(env, e);
            if (item->type == C_TYPE_EXCEPTION) return item;
            av_list_add(l, item);
        }
        return &l->base.base;
    }
    if (strcmp(type, "map") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_OBJECT) return wrong_type("value", "JSONObject", val);
        av_map *m = av_map_new();
        for (size_t i = 0; i < val->count; i++) {
            const js_value *e = val->items[i];
            if (e->kind != JS_OBJECT) {
                char msg[256];
                snprintf(msg, sizeof(msg), "json.unpack(): Malformed JSON, map value for key "
                         "'%s' expecting JSON object but found '%s' instead.",
                         val->keys[i], js_java_class(e));
                return thrown_msg(msg);
            }
            av_type *item = unpack_node(env, e);
            if (item->type == C_TYPE_EXCEPTION) return item;
            av_map_put(m, val->keys[i], item);
        }
        return &m->base.base;
    }
    if (strcmp(type, "Date") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_STRING) return wrong_type("value", "String (ISO 8601)", val);
        return av_date_from_iso(env, val->str);
    }
    if (strcmp(type, "Buffer") == 0) {
        if (val == NULL) return missing_value();
        if (val->kind != JS_STRING) return wrong_type("value", "String (Base64 Encoded)", val);
        /* ABase64.decodeSafe: hex, then base64. */
        size_t hlen = 0;
        char herr[96];
        unsigned char *hb = codec_hex_decode_msg(val->str, &hlen, herr, sizeof(herr));
        if (hb == NULL) {
            char m[192];
            snprintf(m, sizeof(m), "json.unpack(): Getting value for key 'value'. Exception "
                     "unpacking String (Base64 Encoded): %s", herr);
            return thrown_msg(m);
        }
        char *b64 = AUSSOM_ALLOC_ATOMIC(hlen + 1);
        memcpy(b64, hb, hlen);
        b64[hlen] = '\0';
        size_t blen = 0;
        unsigned char *bytes = codec_base64_decode(b64, &blen);
        if (blen > INT_MAX) return stdlib_array_too_large();
        return av_buffer_create(env, bytes, (int)blen);
    }

    /* A user object. */
    ast_class *cls = engine_resolve_class(environment_engine(env), type);
    if (cls == NULL) {
        size_t n = strlen(type) + 64;
        char *m = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(m, n, "json.unpack(): Malformed JSON, type '%s' unknown.", type);
        return thrown_msg(m);
    }
    const js_value *members = js_get(obj, "members");
    if (members == NULL) return missing_value();
    if (members->kind != JS_OBJECT) return wrong_type("members", "JSONObject", members);
    av_type *inst = class_instantiate(env, cls, false, av_list_new());
    if (av_is_ex(inst)) return inst;
    av_object *o = (av_object *)inst;

    /* Snapshot the instance's member keys before mutating the map. */
    av_strmap *mm = av_object_members(o);
    av_strmap_lock(mm);
    size_t mn = av_strmap_size_locked(mm);
    const char **keys = AUSSOM_ALLOC(sizeof(char *) * (mn > 0 ? mn : 1));
    for (size_t i = 0; i < mn; i++) keys[i] = av_strmap_key_at_locked(mm, i);
    av_strmap_unlock(mm);

    for (size_t i = 0; i < mn; i++) {
        const js_value *mv = js_get(members, keys[i]);
        if (mv == NULL) {
            size_t n = strlen(type) + strlen(keys[i]) + 80;
            char *m = AUSSOM_ALLOC_ATOMIC(n);
            snprintf(m, n, "json.unpack(): Malformed JSON, object '%s' missing member '%s'.",
                     type, keys[i]);
            return thrown_msg(m);
        }
        if (mv->kind != JS_OBJECT) {
            /* Java names the members object's class, not the member's. */
            size_t n = strlen(keys[i]) + 128;
            char *m = AUSSOM_ALLOC_ATOMIC(n);
            snprintf(m, n, "json.unpack(): Malformed JSON, expecting '%s' to be a JSON object "
                     "but found 'org.json.simple.JSONObject' instead.", keys[i]);
            return thrown_msg(m);
        }
        av_type *uv = unpack_node(env, mv);
        if (uv->type == C_TYPE_EXCEPTION) return uv;
        av_object_add_member(o, keys[i], uv);
    }
    return inst;
}

/* AJson.unpack: the parsed text is cast to a JSONObject. */
static av_type *json_unpack(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *jstr = av_list_get_string(args, 0);
    if (jstr == NULL) return &av_exception_new("json_unpack: argument 1 must be a string.")->base.base;
    const char *err;
    js_value *root = js_parse(jstr, &err);
    if (root == NULL) return thrown(err);
    if (root->kind == JS_NULL) {
        return thrown("java.lang.NullPointerException: Cannot invoke "
                      "\"org.json.simple.JSONObject.containsKey(Object)\" because \"obj\" is null");
    }
    if (root->kind != JS_OBJECT) return cast_error(root, "org.json.simple.JSONObject");
    return unpack_node(env, root);
}

const aussom_extern_method stdlib_ajson_methods[] = {
    { "parse", json_parse },
    { "escape", json_escape },
    { "unpack", json_unpack },
    { NULL, NULL }
};
