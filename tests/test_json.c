/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Tests for the JSON reader (json_simple.h) and writer (av_json.h) as a
 * host reaches them, through the public headers alone.
 */

#include <string.h>

#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/av_json.h"
#include "aussom/json_simple.h"
#include "greatest.h"

TEST js_parse_reads_an_object(void)
{
    const char *err = "unset";
    js_value *v = js_parse("{\"a\":1,\"b\":[true,\"x\"],\"c\":null}", &err);
    ASSERT(v != NULL);
    ASSERT_EQ(NULL, err);
    ASSERT_EQ(JS_OBJECT, v->kind);

    js_value *a = js_get(v, "a");
    ASSERT(a != NULL);
    ASSERT_EQ(JS_LONG, a->kind);
    ASSERT_EQ(1, a->lval);

    js_value *b = js_get(v, "b");
    ASSERT(b != NULL);
    ASSERT_EQ(JS_ARRAY, b->kind);
    ASSERT_EQ(2, b->count);
    ASSERT_EQ(JS_BOOL, b->items[0]->kind);
    ASSERT_STR_EQ("x", b->items[1]->str);

    js_value *c = js_get(v, "c");
    ASSERT(c != NULL);
    ASSERT_EQ(JS_NULL, c->kind);
    ASSERT_EQ(NULL, js_get(v, "missing"));
    PASS();
}

TEST js_parse_reports_an_error(void)
{
    const char *err = NULL;
    ASSERT_EQ(NULL, js_parse("nope", &err));
    ASSERT_STR_EQ("Unexpected character (n) at position 0.", err);
    PASS();
}

TEST av_to_json_writes_a_map(void)
{
    av_map *m = av_map_new();
    av_map_put(m, "n", &av_int_new(3)->base.base);
    av_map_put(m, "s", &av_string_new("hi")->base.base);
    av_type *out = av_to_json(&m->base.base);
    ASSERT_EQ(C_TYPE_STRING, out->type);
    const char *text = av_string_get((av_string *)out);
    ASSERT(strstr(text, "\"n\":3") != NULL);
    ASSERT(strstr(text, "\"s\":\"hi\"") != NULL);
    PASS();
}

SUITE(json)
{
    RUN_TEST(js_parse_reads_an_object);
    RUN_TEST(js_parse_reports_an_error);
    RUN_TEST(av_to_json_writes_a_map);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    aussom_gc_init();

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(json);
    GREATEST_MAIN_END();
}
