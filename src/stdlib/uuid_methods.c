/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `uuid` class (com.aussom.stdlib.AUuid).
 *   get()       -> a random version-4 UUID string.
 *   getSecure() -> base64.encode(SHA1(randomInt)) where base64.encode
 *                  is hex(base64(bytes)), matching ABase64.encode.
 *
 * Randomness comes from /dev/urandom when available, else rand().
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/extern_registry.h"
#include "codec.h"
#include "stdlib_internal.h"

static void rand_bytes(unsigned char *buf, size_t n)
{
    FILE *fp = fopen("/dev/urandom", "rb");
    if (fp != NULL) {
        size_t got = fread(buf, 1, n, fp);
        fclose(fp);
        if (got == n) return;
    }
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)clock()); seeded = true; }
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(rand() & 0xff);
}

static av_type *uuid_get(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self; (void)args;
    unsigned char b[16];
    rand_bytes(b, 16);
    b[6] = (unsigned char)(0x40 | (b[6] & 0x0f));   /* version 4 */
    b[8] = (unsigned char)(0x80 | (b[8] & 0x3f));   /* variant 1 */
    char out[37];
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return &av_string_new(out)->base.base;
}

static av_type *uuid_getSecure(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)self; (void)args;
    unsigned char rb[4];
    rand_bytes(rb, 4);
    int32_t r = (int32_t)((uint32_t)rb[0] << 24 | (uint32_t)rb[1] << 16 |
                          (uint32_t)rb[2] << 8 | rb[3]);
    char num[16];
    int nlen = snprintf(num, sizeof(num), "%d", r);

    unsigned char digest[20];
    codec_sha1((const unsigned char *)num, (size_t)nlen, digest);

    char *b64 = codec_base64_encode(digest, 20);
    char *hx = codec_hex_encode((const unsigned char *)b64, strlen(b64));
    return &av_string_new(hx)->base.base;
}

const aussom_extern_method stdlib_auuid_methods[] = {
    { "get", uuid_get },
    { "getSecure", uuid_getSecure },
    { NULL, NULL }
};
