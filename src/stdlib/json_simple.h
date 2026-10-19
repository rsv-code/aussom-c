/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_JSON_SIMPLE_H
#define AUSSOM_JSON_SIMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A port of json-simple 1.1.1's JSONParser and its JFlex lexer, the
 * parser Java's json.parse and json.unpack use. It keeps json-simple's
 * leniency (commas are skipped inside arrays and objects, so [1,] and
 * [,1] read as [1]) and its strictness (text after the value is an
 * error), and it gives the same error text.
 *
 * Values keep the Java type json-simple produced for them, since
 * json.unpack checks and reports those types. */

typedef enum {
    JS_STRING,   /* java.lang.String */
    JS_LONG,     /* java.lang.Long */
    JS_DOUBLE,   /* java.lang.Double */
    JS_BOOL,     /* java.lang.Boolean */
    JS_NULL,     /* null */
    JS_OBJECT,   /* org.json.simple.JSONObject */
    JS_ARRAY     /* org.json.simple.JSONArray */
} js_kind;

typedef struct js_value {
    js_kind kind;
    char *str;                 /* JS_STRING */
    int64_t lval;              /* JS_LONG */
    double dval;               /* JS_DOUBLE */
    bool bval;                 /* JS_BOOL */
    size_t count;              /* JS_OBJECT / JS_ARRAY entries */
    size_t cap;
    char **keys;               /* JS_OBJECT keys, first-seen order */
    struct js_value **items;   /* JS_OBJECT values / JS_ARRAY items */
} js_value;

/* Parses `text`. On success returns the root value (JS_NULL for the
 * text "null") and sets *err to NULL. On failure returns NULL and sets
 * *err to what the thrown Java exception's toString() gives: a
 * ParseException's own text ("Unexpected character (b) at position
 * 1."), "java.lang.NumberFormatException: ..." for an integer too large
 * for a long, or "java.lang.Error: Error: could not match input". */
js_value *js_parse(const char *text, const char **err);

/* The value for `key` in an object, or NULL when the key is absent. A
 * key that is present with a JSON null gives a JS_NULL value. */
js_value *js_get(const js_value *obj, const char *key);

/* getClass().getName() of the Java object for `v`. */
const char *js_java_class(const js_value *v);

#endif /* AUSSOM_JSON_SIMPLE_H */
