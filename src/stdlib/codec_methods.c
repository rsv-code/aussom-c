/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `base64` and `hex` classes (com.aussom.stdlib.ABase64 /
 * AHex). hex operates on Buffer objects.
 *
 * base64 has two axes: what the value IS (string vs Buffer) and how it
 * is ENCODED (standard base64 vs the "safe" hex-of-base64 form, whose
 * output is only 0-9 and a-f and so needs no escaping in URLs,
 * filenames or headers).
 *
 *   encode/decode           string  <-> standard base64
 *   encodeBinary/decodeBinary Buffer <-> standard base64
 *   encodeSafe/decodeSafe   string  <-> safe (hex of base64)
 *
 * This replaced the 1.2.x API, where encode/decode took a Buffer and
 * silently produced the safe form while encodeRaw/decodeRaw were the
 * only plain-base64 path. The safe form is still what Buffer.toJson,
 * pack, and uuid.getSecure emit.
 */

#include <limits.h>
#include <string.h>

#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/extern_registry.h"
#include "codec.h"
#include "stdlib_internal.h"

static av_type *codec_exc(const char *msg) { return &av_exception_new(msg)->base.base; }

/* Reads the Buffer argument (arg 0) into bytes/len, or returns an
 * exception with `not_buffer_msg` when it isn't a Buffer. */
static buf_state *arg_buffer(environment *env, av_list *args, const char *not_buffer_msg,
                             av_type **err)
{
    *err = NULL;
    buf_state *b = av_buffer_state(env, av_list_get(args, 0));
    if (b == NULL) *err = codec_exc(not_buffer_msg);
    return b;
}

/* --- hex --------------------------------------------------------- */

static av_type *hex_encode(av_type *self, environment *env, av_list *args)
{
    (void)self;
    av_type *err;
    buf_state *b = arg_buffer(env, args, "hex.encode(): External object is null or not of type Buffer.", &err);
    if (err != NULL) return err;
    return &av_string_new(codec_hex_encode(b->data, (size_t)b->size))->base.base;
}

static av_type *hex_decode(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *data = av_list_get_string(args, 0);
    if (data == NULL) return codec_exc("hex_decode: argument 1 must be a string.");
    size_t len = 0;
    unsigned char *bytes = codec_hex_decode(data, &len);
    if (bytes == NULL) return codec_exc("hex.decode(): Class 'Buffer'.");
    if (len > INT_MAX) return stdlib_array_too_large();
    return av_buffer_create(env, bytes, (int)len);
}

/* --- base64 ------------------------------------------------------ */

/* encode(string) -> string : standard base64 of the UTF-8 bytes. */
static av_type *base64_encode(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    const char *s = av_list_get_string(args, 0);
    if (s == NULL) return codec_exc("base64_encode: argument 1 must be a string.");
    size_t n;
    unsigned char *bytes = codec_utf8_bytes(s, &n);
    return &av_string_new(codec_base64_encode(bytes, n))->base.base;
}

/* decode(string) -> string : standard base64 -> UTF-8 string. */
static av_type *base64_decode(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    const char *data = av_list_get_string(args, 0);
    if (data == NULL) return codec_exc("base64_decode: argument 1 must be a string.");
    size_t blen = 0;
    unsigned char *bytes = codec_base64_decode(data, &blen);
    return &av_string_new(codec_utf8_string(bytes, blen))->base.base;
}

/* encodeBinary(Buffer) -> string : standard base64 of the bytes. */
static av_type *base64_encodeBinary(av_type *self, environment *env, av_list *args)
{
    (void)self;
    av_type *err;
    buf_state *b = arg_buffer(env, args,
        "base64.encodeBinary(): Argument is null or not of type Buffer.", &err);
    if (err != NULL) return err;
    return &av_string_new(codec_base64_encode(b->data, (size_t)b->size))->base.base;
}

/* decodeBinary(string) -> Buffer : standard base64 -> bytes. */
static av_type *base64_decodeBinary(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *data = av_list_get_string(args, 0);
    if (data == NULL) return codec_exc("base64_decodeBinary: argument 1 must be a string.");
    size_t blen = 0;
    unsigned char *bytes = codec_base64_decode(data, &blen);
    if (blen > INT_MAX) return stdlib_array_too_large();
    return av_buffer_create(env, bytes, (int)blen);
}

/* encodeSafe(string) -> string : hex of base64, so the output uses only
 * 0-9 and a-f and needs no escaping anywhere. */
static av_type *base64_encodeSafe(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    const char *s = av_list_get_string(args, 0);
    if (s == NULL) return codec_exc("base64_encodeSafe: argument 1 must be a string.");
    size_t n;
    unsigned char *bytes = codec_utf8_bytes(s, &n);
    char *b64 = codec_base64_encode(bytes, n);
    return &av_string_new(codec_hex_encode((const unsigned char *)b64, strlen(b64)))->base.base;
}

/* decodeSafe(string) -> string : safe form -> UTF-8 string. */
static av_type *base64_decodeSafe(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    const char *data = av_list_get_string(args, 0);
    if (data == NULL) return codec_exc("base64_decodeSafe: argument 1 must be a string.");
    size_t hlen = 0;
    char herr[96];
    unsigned char *hb = codec_hex_decode_msg(data, &hlen, herr, sizeof(herr));
    if (hb == NULL) {
        char m[160];
        snprintf(m, sizeof(m), "base64.decodeSafe(): %s", herr);
        return codec_exc(m);
    }
    /* hex-decode yields raw base64 text; build a NUL-terminated copy
     * for the base64 decoder. */
    char *b64 = AUSSOM_ALLOC_ATOMIC(hlen + 1);
    memcpy(b64, hb, hlen);
    b64[hlen] = '\0';
    size_t blen = 0;
    unsigned char *bytes = codec_base64_decode(b64, &blen);
    return &av_string_new(codec_utf8_string(bytes, blen))->base.base;
}

const aussom_extern_method stdlib_ahex_methods[] = {
    { "encode", hex_encode },
    { "decode", hex_decode },
    { NULL, NULL }
};

const aussom_extern_method stdlib_abase64_methods[] = {
    { "encode", base64_encode },
    { "decode", base64_decode },
    { "encodeBinary", base64_encodeBinary },
    { "decodeBinary", base64_decodeBinary },
    { "encodeSafe", base64_encodeSafe },
    { "decodeSafe", base64_decodeSafe },
    { NULL, NULL }
};
