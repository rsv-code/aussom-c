/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#if defined(_WIN32)
/* rand_s is declared by stdlib.h only when this is set first. */
#  define _CRT_RAND_S
#elif defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* arc4random_buf is hidden by a strict _POSIX_C_SOURCE. The Makefile
 * sets this for macOS; an Xcode build of the library may not. */
#  define _DARWIN_C_SOURCE
#endif

#include "aussom/av_strmap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aussom/aussom_gc.h"

#define AV_STRMAP_INITIAL_BUCKETS 16
#define AV_STRMAP_MAX_LOAD 0.75

/* Keys are hashed with SipHash-1-3 under a random per-process key.
 *
 * Buckets are chained lists picked by the hash modulo a power of two.
 * With a fixed hash anyone can compute, ahead of time, many keys that
 * land in one bucket, and then each insert walks the whole chain: a
 * map built from them (json.parse of untrusted text, for example)
 * costs the square of the key count. Java's ConcurrentHashMap guards
 * against this by turning a long chain into a balanced tree. The C
 * port keys the hash instead: without the key, colliding strings
 * cannot be chosen. A seeded FNV-1a would not do, because the low bits
 * of its result depend only on the low bits of the seed and the input.
 *
 * Map iteration follows insertion order (the order array), never the
 * hash, so the key changes nothing a script can see.
 *
 * A frame's local variables (the unlocked maps) keep plain FNV-1a. Their
 * keys are names written in the script's own source, so only the
 * script's author could make them collide, and a variable is looked up
 * on every read: keying those maps too made the interpreter about 8%
 * slower. */
static uint64_t hash_k0, hash_k1;
static pthread_once_t hash_key_once = PTHREAD_ONCE_INIT;

static void hash_key_init(void)
{
    uint64_t k[2] = { 0, 0 };
    bool ok = false;
#if defined(_WIN32)
    unsigned int r[4];
    ok = true;
    for (int i = 0; i < 4; i++) if (rand_s(&r[i]) != 0) ok = false;
    if (ok) {
        k[0] = (uint64_t)r[0] << 32 | r[1];
        k[1] = (uint64_t)r[2] << 32 | r[3];
    }
#elif defined(__APPLE__) || defined(__ANDROID__) || defined(__FreeBSD__)
    arc4random_buf(k, sizeof(k));
    ok = true;
#else
    FILE *fp = fopen("/dev/urandom", "rb");
    if (fp != NULL) {
        ok = fread(k, 1, sizeof(k), fp) == sizeof(k);
        fclose(fp);
    }
#endif
    if (!ok) {
        /* No OS randomness: the time and an address still keep the key
         * from being known ahead of time. */
        k[0] = (uint64_t)time(NULL) ^ ((uint64_t)clock() << 32);
        k[1] = (uint64_t)(uintptr_t)&k ^ ((uint64_t)(uintptr_t)&hash_k0 << 17);
    }
    hash_k0 = k[0];
    hash_k1 = k[1];
}

#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND do { \
        v0 += v1; v1 = SIP_ROTL(v1, 13); v1 ^= v0; v0 = SIP_ROTL(v0, 32); \
        v2 += v3; v3 = SIP_ROTL(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = SIP_ROTL(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = SIP_ROTL(v1, 17); v1 ^= v2; v2 = SIP_ROTL(v2, 32); \
    } while (0)

static size_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return (size_t)h;
}

/* The key is set by strmap_new, which runs before any key of that map
 * is hashed. */
static size_t sip_hash(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t len = strlen(s);
    uint64_t v0 = hash_k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = hash_k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = hash_k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = hash_k1 ^ 0x7465646279746573ULL;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t m;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        memcpy(&m, p + i, 8);
#else
        m = 0;
        for (int j = 7; j >= 0; j--) m = (m << 8) | p[i + j];
#endif
        v3 ^= m; SIP_ROUND; v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t j = 0; i + j < len; j++) b |= (uint64_t)p[i + j] << (8 * j);
    v3 ^= b; SIP_ROUND; v0 ^= b;
    v2 ^= 0xff;
    SIP_ROUND; SIP_ROUND; SIP_ROUND;
    return (size_t)(v0 ^ v1 ^ v2 ^ v3);
}

static size_t key_hash(const av_strmap *m, const char *key)
{
    return m->synchronized ? sip_hash(key) : fnv1a(key);
}

static av_strmap *strmap_new(bool synchronized)
{
    pthread_once(&hash_key_once, hash_key_init);
    av_strmap *m = AUSSOM_ALLOC(sizeof(*m));
    m->bucket_count = AV_STRMAP_INITIAL_BUCKETS;
    m->buckets = AUSSOM_ALLOC(m->bucket_count * sizeof(*m->buckets));
    for (size_t i = 0; i < m->bucket_count; i++) m->buckets[i] = NULL;
    m->count = 0;
    m->order = NULL;
    m->order_count = 0;
    m->order_capacity = 0;
    m->synchronized = synchronized;
    if (synchronized) pthread_mutex_init(&m->lock, NULL);
    return m;
}

av_strmap *av_strmap_new(void)          { return strmap_new(true); }
av_strmap *av_strmap_new_unlocked(void) { return strmap_new(false); }

/* Lock helpers honour the per-map `synchronized` flag: unlocked maps
 * (frame-private locals) skip the mutex entirely. */
static void sm_lock(av_strmap *m)   { if (m->synchronized) pthread_mutex_lock(&m->lock); }
static void sm_unlock(av_strmap *m) { if (m->synchronized) pthread_mutex_unlock(&m->lock); }

/* Caller holds the lock for all helpers below. */

static void order_append(av_strmap *m, av_strmap_entry *e)
{
    if (m->order_count == m->order_capacity) {
        size_t nc = (m->order_capacity == 0) ? 8 : m->order_capacity * 2;
        m->order = AUSSOM_REALLOC(m->order, nc * sizeof(*m->order));
        m->order_capacity = nc;
    }
    m->order[m->order_count++] = e;
}

static void order_remove(av_strmap *m, av_strmap_entry *e)
{
    for (size_t i = 0; i < m->order_count; i++) {
        if (m->order[i] == e) {
            for (size_t j = i; j + 1 < m->order_count; j++) {
                m->order[j] = m->order[j + 1];
            }
            m->order_count--;
            m->order[m->order_count] = NULL;
            return;
        }
    }
}

static void rehash(av_strmap *m)
{
    size_t nc = m->bucket_count * 2;
    av_strmap_entry **nb = AUSSOM_ALLOC(nc * sizeof(*nb));
    for (size_t i = 0; i < nc; i++) nb[i] = NULL;
    /* Re-link every entry into the new bucket array. Iterate the
     * insertion-order view so entry identity is preserved. */
    for (size_t i = 0; i < m->order_count; i++) {
        av_strmap_entry *e = m->order[i];
        size_t b = key_hash(m, e->key) % nc;
        e->next = nb[b];
        nb[b] = e;
    }
    m->buckets = nb;
    m->bucket_count = nc;
}

static av_strmap_entry *find_entry(av_strmap *m, const char *key, size_t *bucket_out)
{
    size_t b = key_hash(m, key) % m->bucket_count;
    if (bucket_out != NULL) *bucket_out = b;
    for (av_strmap_entry *e = m->buckets[b]; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

void av_strmap_put(av_strmap *m, const char *key, av_type *value)
{
    sm_lock(m);
    size_t b;
    av_strmap_entry *e = find_entry(m, key, &b);
    if (e != NULL) {
        e->value = value;   /* update keeps insertion position */
    } else {
        if ((double)(m->count + 1) > AV_STRMAP_MAX_LOAD * (double)m->bucket_count) {
            rehash(m);
            b = key_hash(m, key) % m->bucket_count;
        }
        e = AUSSOM_ALLOC(sizeof(*e));
        e->key = AUSSOM_STRDUP(key);
        e->value = value;
        e->next = m->buckets[b];
        m->buckets[b] = e;
        m->count++;
        order_append(m, e);
    }
    sm_unlock(m);
}

av_type *av_strmap_get(av_strmap *m, const char *key)
{
    sm_lock(m);
    av_strmap_entry *e = find_entry(m, key, NULL);
    av_type *r = (e != NULL) ? e->value : NULL;
    sm_unlock(m);
    return r;
}

bool av_strmap_contains(av_strmap *m, const char *key)
{
    sm_lock(m);
    bool r = find_entry(m, key, NULL) != NULL;
    sm_unlock(m);
    return r;
}

av_type *av_strmap_remove(av_strmap *m, const char *key)
{
    sm_lock(m);
    size_t b = key_hash(m, key) % m->bucket_count;
    av_strmap_entry *prev = NULL;
    av_type *removed = NULL;
    for (av_strmap_entry *e = m->buckets[b]; e != NULL; prev = e, e = e->next) {
        if (strcmp(e->key, key) == 0) {
            if (prev == NULL) m->buckets[b] = e->next;
            else prev->next = e->next;
            order_remove(m, e);
            m->count--;
            removed = e->value;
            break;
        }
    }
    sm_unlock(m);
    return removed;
}

size_t av_strmap_size(av_strmap *m)
{
    sm_lock(m);
    size_t n = m->count;
    sm_unlock(m);
    return n;
}

void av_strmap_clear(av_strmap *m)
{
    sm_lock(m);
    for (size_t i = 0; i < m->bucket_count; i++) m->buckets[i] = NULL;
    m->count = 0;
    m->order_count = 0;
    sm_unlock(m);
}

void av_strmap_lock(av_strmap *m)   { sm_lock(m); }
void av_strmap_unlock(av_strmap *m) { sm_unlock(m); }

size_t av_strmap_size_locked(const av_strmap *m) { return m->count; }

const char *av_strmap_key_at_locked(const av_strmap *m, size_t i)
{
    return (i < m->order_count) ? m->order[i]->key : NULL;
}

av_type *av_strmap_value_at_locked(const av_strmap *m, size_t i)
{
    return (i < m->order_count) ? m->order[i]->value : NULL;
}

size_t av_strmap_snapshot(av_strmap *m, const char ***keys, av_type ***values)
{
    av_strmap_lock(m);
    size_t n = av_strmap_size_locked(m);
    const char **k = NULL;
    av_type **v = NULL;
    if (n > 0) {
        k = AUSSOM_ALLOC(n * sizeof(*k));
        v = AUSSOM_ALLOC(n * sizeof(*v));
        for (size_t i = 0; i < n; i++) {
            k[i] = av_strmap_key_at_locked(m, i);
            v[i] = av_strmap_value_at_locked(m, i);
        }
    }
    av_strmap_unlock(m);
    *keys = k;
    *values = v;
    return n;
}
