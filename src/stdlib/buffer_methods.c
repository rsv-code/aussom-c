/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The Buffer extern type (com.aussom.stdlib.ABuffer): a fixed-size
 * byte array with independent read/write cursors and big/little endian
 * typed access.
 *
 * Two deviations from the Java original:
 *   - Java's typed read/write bounds checks are too loose (they permit
 *     an index one short of the element width and then rely on the JVM
 *     throwing ArrayIndexOutOfBounds). In C that is an out-of-bounds
 *     read/write, so we use a correct check (index + width <= size).
 *     The exception text is unchanged, so every non-crashing case
 *     behaves identically.
 *   - Charsets are converted by charset.c, which supports a subset of
 *     Java's (see charset.h); others raise the same "Unsopported
 *     encoding" exception Java would.
 *
 * The getDouble read-cursor advance of 4 (not 8) is a Java bug,
 * reproduced here for byte-for-byte parity.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/av_object.h"
#include "aussom/ast_eval.h"
#include "aussom/engine.h"
#include "aussom/extern_registry.h"
#include "charset.h"
#include "codec.h"
#include "stdlib_internal.h"

/* --- helpers ----------------------------------------------------- */

/* Validated access to the receiver's Buffer state.
 *
 * A script can declare its own `extern class ... : com.aussom.stdlib.ABuffer`
 * or redefine the `Buffer` class name and then
 * call these methods on an object that was never initialized as a
 * Buffer. The C code would read buf_state fields out of a plain
 * av_object header. Mirror av_buffer_state: require that the receiver
 * really is a Buffer and that its extern_object was replaced by a real
 * buf_state, not still pointing at the instance itself (the
 * class_instantiate default, meaning newBuffer never ran). Returns NULL
 * otherwise; every caller turns NULL into a clean exception. */
/* True for an object whose class is backed by ABuffer: Buffer, a
 * class extending it, or an `extern class ... : com.aussom.stdlib.ABuffer`. */
static bool abuffer_class(const ast_class *cd)
{
    if (cd == NULL) return false;
    if (ast_class_instance_of(cd, "Buffer")) return true;
    const char *ref = ast_class_get_extern_class_name(cd);
    return ref != NULL && strcmp(ref, "com.aussom.stdlib.ABuffer") == 0;
}

/* The receiver's Buffer state. Java builds every ABuffer-backed object
 * with ABuffer()'s 1024-byte array before any constructor runs, so an
 * object whose extern object is still itself (newBuffer never ran) gets
 * that default buffer here. Anything that is not ABuffer-backed, or
 * carries another extern class's state, gives NULL. */
static buf_state *buffer_of(av_type *v)
{
    if (v == NULL || v->type != C_TYPE_OBJECT) return NULL;
    av_object *o = (av_object *)v;
    if (!abuffer_class((ast_class *)av_object_get_class_def(o))) return NULL;
    void *eo = av_object_get_extern_object(o);
    if (eo == NULL || eo == (void *)o) {
        buf_state *b = AUSSOM_ALLOC(sizeof(buf_state));
        b->magic = BUF_MAGIC;
        b->data = AUSSOM_ALLOC_ATOMIC(1024);
        memset(b->data, 0, 1024);
        b->size = 1024;
        b->wcur = 0;
        b->rcur = 0;
        av_object_set_extern_object(o, b);
        return b;
    }
    if (((buf_state *)eo)->magic != BUF_MAGIC) return NULL;
    return (buf_state *)eo;
}

static buf_state *self_buf(av_type *self, environment *env)
{
    (void)env;
    return buffer_of(self);
}

/* Returned by every Buffer method when self_buf rejects the receiver. */
static av_type *buf_bad_self(void)
{
    return &av_exception_new("Buffer: receiver is not an initialized "
                             "Buffer object.")->base.base;
}

static av_type *buf_exc(const char *msg) { return &av_exception_new(msg)->base.base; }

/* Optional/required arg readers (extern dispatch does NOT fill in
 * declared defaults, so each optional arg is defaulted by hand). */
static int64_t req_int(av_list *a, size_t i) { return av_numeric_int(av_list_get(a, i)); }

/* Narrows a 64-bit index or size to an int, or refuses it.
 *
 * Aussom integers are 64-bit; buffer offsets are not. Casting silently
 * turns 4294967296 into 0 and 4294967295 into -1, and -1 is the "use
 * the cursor" convention here, so a plain cast does not just produce
 * the wrong number -- it changes what the argument means and moves the
 * cursor. Below 2^31 every index behaved correctly and at or above it
 * the script got a plausible answer instead of a range error.
 *
 * Returns NULL and sets *out on success, or the exception to return.
 * -1 still narrows to -1, so the cursor convention at each call site is
 * untouched. */
static av_type *narrow_index(const char *method, int64_t value, int *out)
{
    /* Written before the check so callers never hold an uninitialized
     * index on the refusal path, whatever they do with it. */
    *out = 0;
    if (value < INT_MIN || value > INT_MAX) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Buffer.%s(): Index or size of %lld is out of range.",
                 method, (long long)value);
        /* indexToInt throws. */
        return &av_exception_new_thrown_aussom(msg)->base.base;
    }
    *out = (int)value;
    return NULL;
}

/* narrow_index for a required argument. */
static av_type *req_index(const char *method, av_list *a, size_t i, int *out)
{
    return narrow_index(method, req_int(a, i), out);
}

/* narrow_index for an optional argument, keeping the default when the
 * argument is absent or null. */
static av_type *opt_index(const char *method, av_list *a, size_t i, int dflt,
                          int *out)
{
    if (i >= av_list_size(a)) { *out = dflt; return NULL; }
    av_type *v = av_list_get(a, i);
    if (v->type == C_TYPE_NULL) { *out = dflt; return NULL; }
    return narrow_index(method, av_numeric_int(v), out);
}

static const char *opt_str(av_list *a, size_t i, const char *dflt)
{
    if (i >= av_list_size(a)) return dflt;
    av_type *v = av_list_get(a, i);
    if (v->type != C_TYPE_STRING) return dflt;
    return av_string_get((av_string *)v);
}

/* getByteOrder: BIG unless the string is exactly "little". */
static bool order_big(av_list *a, size_t i)
{
    return strcmp(opt_str(a, i, "big"), "little") != 0;
}

static bool in_bounds(buf_state *b, int index, int width)
{
    /* int64_t, not long: long is 32 bits on Windows, where the sum
     * could wrap. */
    return index >= 0 && (int64_t)index + width <= (int64_t)b->size;
}

static void put_bytes(buf_state *b, int index, uint64_t val, int n, bool big)
{
    if (big) {
        for (int i = 0; i < n; i++) b->data[index + i] = (val >> (8 * (n - 1 - i))) & 0xff;
    } else {
        for (int i = 0; i < n; i++) b->data[index + i] = (val >> (8 * i)) & 0xff;
    }
}

static uint64_t get_bytes(buf_state *b, int index, int n, bool big)
{
    uint64_t v = 0;
    if (big) {
        for (int i = 0; i < n; i++) v = (v << 8) | b->data[index + i];
    } else {
        for (int i = n - 1; i >= 0; i--) v = (v << 8) | b->data[index + i];
    }
    return v;
}

static uint32_t f2b(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    b2f(uint32_t u){ float f;  memcpy(&f, &u, 4); return f; }
static uint64_t d2b(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double   b2d(uint64_t u){ double d;  memcpy(&d, &u, 8); return d; }

/* The charset for a Buffer method's charset argument, looked up the
 * way new String(bytes, name) and String.getBytes(name) do it. NULL
 * when Java would throw UnsupportedEncodingException; its message is
 * the name, which cset_name gives. */
static const cs_charset *charset_for(const char *cset)
{
    const char *name = cs_java_name(cset);
    return cs_name_legal(name) ? cs_lookup(name) : NULL;
}

/* Charset.forName, which _setStringAt calls: NULL with the charset in
 * *cs, or the thrown IllegalCharsetNameException or
 * UnsupportedCharsetException. */
static av_type *charset_forname(const char *cset, const cs_charset **cs)
{
    const char *name = cs_java_name(cset);
    char m[160];
    if (!cs_name_legal(name)) {
        snprintf(m, sizeof(m), "java.nio.charset.IllegalCharsetNameException: %s", name);
        return &av_exception_new_thrown(m)->base.base;
    }
    *cs = cs_lookup(name);
    if (*cs == NULL) {
        snprintf(m, sizeof(m), "java.nio.charset.UnsupportedCharsetException: %s", name);
        return &av_exception_new_thrown(m)->base.base;
    }
    return NULL;
}

/* ABuffer.checkSize: a size must not be negative and must fit a Java
 * array. Returned, not thrown. */
static av_type *check_size(const char *method, int64_t size)
{
    char msg[160];
    if (size < 0) {
        snprintf(msg, sizeof(msg), "Buffer.%s(): Size must not be negative, found %lld.",
                 method, (long long)size);
        return buf_exc(msg);
    }
    if (size > INT_MAX) {
        snprintf(msg, sizeof(msg), "Buffer.%s(): Size of %lld is larger than a Java "
                 "array can address (%d).", method, (long long)size, INT_MAX);
        return buf_exc(msg);
    }
    return NULL;
}

/* An exception Java's code throws (an aussomException), as opposed to
 * one it returns. */
static av_type *buf_thrown(const char *msg)
{
    return &av_exception_new_thrown_aussom(msg)->base.base;
}

/* System.arraycopy's own checks, in its order, as the thrown
 * ArrayIndexOutOfBoundsException. NULL when the copy is in range. */
static av_type *arraycopy_check(int src_len, int src_pos, int dst_len, int dst_pos, int length)
{
    char m[160];
    if (src_pos < 0)
        snprintf(m, sizeof(m), "arraycopy: source index %d out of bounds for byte[%d]", src_pos, src_len);
    else if (dst_pos < 0)
        snprintf(m, sizeof(m), "arraycopy: destination index %d out of bounds for byte[%d]", dst_pos, dst_len);
    else if (length < 0)
        snprintf(m, sizeof(m), "arraycopy: length %d is negative", length);
    else if ((int64_t)src_pos + length > src_len)
        snprintf(m, sizeof(m), "arraycopy: last source index %lld out of bounds for byte[%d]",
                 (long long)src_pos + length, src_len);
    else if ((int64_t)dst_pos + length > dst_len)
        snprintf(m, sizeof(m), "arraycopy: last destination index %lld out of bounds for byte[%d]",
                 (long long)dst_pos + length, dst_len);
    else
        return NULL;
    char full[224];
    snprintf(full, sizeof(full), "java.lang.ArrayIndexOutOfBoundsException: %s", m);
    return &av_exception_new_thrown(full)->base.base;
}

/* The upper-case charset name Java's messages show (toUpperCase, with
 * '_' as '-'). */
static const char *cset_name(const char *cset) { return cs_java_name(cset); }

/* The UTF-16 units of string argument i, or NULL when it is not a
 * string. */
static const uint16_t *str_units(av_list *a, size_t i, size_t *n)
{
    av_type *v = i < av_list_size(a) ? av_list_get(a, i) : NULL;
    if (v == NULL || v->type != C_TYPE_STRING) return NULL;
    return av_string_units((av_string *)v, n);
}

/* --- lifecycle --------------------------------------------------- */

static av_type *buf_newBuffer(av_type *self, environment *env, av_list *args)
{
    (void)env;
    /* Checked as a 64-bit value before anything is allocated. A
     * negative size, or one too large to address, used to be narrowed
     * and quietly produce a Buffer of some other size: new Buffer(-1)
     * gave an empty one and new Buffer(4294967296) gave an empty one
     * too, neither reporting anything. */
    int64_t req = req_int(args, 0);
    av_type *bad = check_size("newBuffer", req);
    if (bad != NULL) return bad;
    int size = (int)req;

    buf_state *b = AUSSOM_ALLOC(sizeof(buf_state));
    b->magic = BUF_MAGIC;
    b->data = AUSSOM_ALLOC_ATOMIC(size > 0 ? (size_t)size : 1);
    memset(b->data, 0, size > 0 ? (size_t)size : 1);
    b->size = size;
    b->wcur = 0;
    b->rcur = 0;
    av_object_set_extern_object((av_object *)self, b);
    return &av_null_new()->base.base;   /* Java newBuffer returns null */
}

static av_type *buf_size(av_type *self, environment *env, av_list *args)
{
    (void)args;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    return &av_int_new(b->size)->base.base;
}

static av_type *buf_clear(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    memset(b->data, 0, (size_t)b->size);
    b->rcur = 0;
    b->wcur = 0;
    return self;
}

static av_type *buf_writeSeek(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    int index;
    av_type *bad = req_index("writeSeek", args, 0, &index);
    if (bad != NULL) return bad;
    if (index >= 0 && index < b->size) { b->wcur = index; return self; }
    return buf_exc("Buffer.writeSeek(): Index out of bounds.");
}

static av_type *buf_readSeek(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    int index;
    av_type *bad = req_index("readSeek", args, 0, &index);
    if (bad != NULL) return bad;
    if (index >= 0 && index < b->size) { b->rcur = index; return self; }
    return buf_exc("Buffer.readSeek(): Index out of bounds.");
}

static av_type *buf_getWriteCursor(av_type *self, environment *env, av_list *args)
{
    (void)args;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    return &av_int_new(b->wcur)->base.base;
}

static av_type *buf_getReadCursor(av_type *self, environment *env, av_list *args)
{
    (void)args;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    return &av_int_new(b->rcur)->base.base;
}

/* --- numeric writes (add* use+advance write cursor; set* explicit) - */

/* width: 1..8; for floats pass the already-encoded bit pattern. */
static av_type *write_at(buf_state *b, int index, uint64_t bits, int width,
                         bool big, const char *oob_msg)
{
    /* The _setX helpers throw. */
    if (!in_bounds(b, index, width)) return buf_thrown(oob_msg);
    put_bytes(b, index, bits, width, big);
    return NULL;
}

static av_type *add_int_like(av_type *self, environment *env, av_list *args, int width,
                             bool has_order, uint64_t bits_override,
                             bool use_override, const char *oob_msg)
{
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    uint64_t bits = use_override ? bits_override : (uint64_t)req_int(args, 0);
    bool big = has_order ? order_big(args, 1) : true;
    av_type *e = write_at(b, b->wcur, bits, width, big, oob_msg);
    if (e != NULL) return e;
    b->wcur += width;
    return self;
}

static av_type *buf_addByte(av_type *self, environment *env, av_list *args)
{ (void)env; return add_int_like(self, env, args, 1, false, 0, false, "Buffer.setByte(): Index out of bounds."); }

static av_type *buf_addUByte(av_type *self, environment *env, av_list *args)
{
    (void)env;
    uint64_t bits = (uint64_t)(req_int(args, 0) & 0xff);
    return add_int_like(self, env, args, 1, false, bits, true, "Buffer.setUByte(): Index out of bounds.");
}

static av_type *buf_addShort(av_type *self, environment *env, av_list *args)
{ (void)env; return add_int_like(self, env, args, 2, true, 0, false, "Buffer.setShort(): Index out of bounds."); }

static av_type *buf_addUShort(av_type *self, environment *env, av_list *args)
{ (void)env; return add_int_like(self, env, args, 2, true, 0, false, "Buffer.setUShort(): Index out of bounds."); }

static av_type *buf_addInt(av_type *self, environment *env, av_list *args)
{ (void)env; return add_int_like(self, env, args, 4, true, 0, false, "Buffer.setInt(): Index out of bounds."); }

static av_type *buf_addUInt(av_type *self, environment *env, av_list *args)
{ (void)env; return add_int_like(self, env, args, 4, true, 0, false, "Buffer.setUInt(): Index out of bounds."); }

static av_type *buf_addLong(av_type *self, environment *env, av_list *args)
{ (void)env; return add_int_like(self, env, args, 8, true, 0, false, "Buffer.setLong(): Index out of bounds."); }

static av_type *buf_addFloat(av_type *self, environment *env, av_list *args)
{
    (void)env;
    uint64_t bits = f2b((float)av_numeric_double(av_list_get(args, 0)));
    return add_int_like(self, env, args, 4, true, bits, true, "Buffer.setFloat(): Index out of bounds.");
}

static av_type *buf_addDouble(av_type *self, environment *env, av_list *args)
{
    (void)env;
    uint64_t bits = d2b(av_numeric_double(av_list_get(args, 0)));
    return add_int_like(self, env, args, 8, true, bits, true, "Buffer.setDouble(): Index out of bounds.");
}

/* set*: explicit index in arg 0, value in arg 1, optional order in 2. */
static av_type *set_int_like(av_type *self, environment *env, av_list *args, int width,
                             bool has_order, uint64_t bits, const char *method,
                             const char *oob_msg)
{
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    int index;
    av_type *bad = req_index(method, args, 0, &index);
    if (bad != NULL) return bad;
    bool big = has_order ? order_big(args, 2) : true;
    /* The value argument is deliberately not range checked: narrowing
     * a value to the field width is what writing a short means, and
     * that is a separate question from an index that changes meaning. */
    av_type *e = write_at(b, index, bits, width, big, oob_msg);
    return (e != NULL) ? e : self;
}

static av_type *buf_setByte(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 1, false, (uint64_t)req_int(args, 1), "setByte", "Buffer.setByte(): Index out of bounds."); }

static av_type *buf_setUByte(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 1, false, (uint64_t)(req_int(args, 1) & 0xff), "setUByte", "Buffer.setUByte(): Index out of bounds."); }

static av_type *buf_setShort(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 2, true, (uint64_t)req_int(args, 1), "setShort", "Buffer.setShort(): Index out of bounds."); }

static av_type *buf_setUShort(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 2, true, (uint64_t)req_int(args, 1), "setUShort", "Buffer.setUShort(): Index out of bounds."); }

static av_type *buf_setInt(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 4, true, (uint64_t)req_int(args, 1), "setInt", "Buffer.setInt(): Index out of bounds."); }

static av_type *buf_setUInt(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 4, true, (uint64_t)req_int(args, 1), "setUInt", "Buffer.setUInt(): Index out of bounds."); }

static av_type *buf_setLong(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 8, true, (uint64_t)req_int(args, 1), "setLong", "Buffer.setLong(): Index out of bounds."); }

static av_type *buf_setFloat(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 4, true, f2b((float)av_numeric_double(av_list_get(args, 1))), "setFloat", "Buffer.setFloat(): Index out of bounds."); }

static av_type *buf_setDouble(av_type *self, environment *env, av_list *args)
{ (void)env; return set_int_like(self, env, args, 8, true, d2b(av_numeric_double(av_list_get(args, 1))), "setDouble", "Buffer.setDouble(): Index out of bounds."); }

/* --- numeric reads ----------------------------------------------- */

/* Resolves the read index from arg 0 (-1 / omitted => read cursor).
 * Sets *use_cursor when the cursor should advance after the read.
 * Returns NULL on success, or the range exception: an index too large
 * for an int used to wrap to a negative number and silently turn into
 * "read at the cursor". */
static av_type *read_index(buf_state *b, av_list *args, const char *method,
                           bool *use_cursor, int *out)
{
    int index;
    av_type *bad = opt_index(method, args, 0, -1, &index);
    if (bad != NULL) return bad;
    if (index < 0) { index = b->rcur; *use_cursor = true; }
    else *use_cursor = false;
    *out = index;
    return NULL;
}

static av_type *buf_getByte(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getByte", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 1)) return buf_thrown("Buffer.getByte(): Index out of bounds.");
    int val = (int)(int8_t)b->data[index];
    if (inc) b->rcur += 1;
    return &av_int_new(val)->base.base;
}

static av_type *buf_getUByte(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getUByte", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 1)) return buf_thrown("Buffer.getUByte(): Index out of bounds.");
    int val = b->data[index] & 0xff;
    if (inc) b->rcur += 1;
    return &av_int_new(val)->base.base;
}

/* Java _getShort and _getUShort both yield the unsigned 0..65535 value. */
static av_type *buf_getShort(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getShort", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 2)) return buf_thrown("Buffer.getShort(): Index out of bounds.");
    int64_t val = (int64_t)(get_bytes(b, index, 2, order_big(args, 1)) & 0xffff);
    if (inc) b->rcur += 2;
    return &av_int_new(val)->base.base;
}

static av_type *buf_getUShort(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getUShort", &inc, &index);
    if (bad != NULL) return bad;
    /* Java getUShort reuses the "getShort" message text. */
    if (!in_bounds(b, index, 2)) return buf_thrown("Buffer.getShort(): Index out of bounds.");
    int64_t val = (int64_t)(get_bytes(b, index, 2, order_big(args, 1)) & 0xffff);
    if (inc) b->rcur += 2;
    return &av_int_new(val)->base.base;
}

static av_type *buf_getInt(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getInt", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 4)) return buf_thrown("Buffer.getInt(): Index out of bounds.");
    int64_t val = (int64_t)(int32_t)(uint32_t)get_bytes(b, index, 4, order_big(args, 1));
    if (inc) b->rcur += 4;
    return &av_int_new(val)->base.base;
}

static av_type *buf_getUInt(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getUInt", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 4)) return buf_thrown("Buffer.getUInt(): Index out of bounds.");
    int64_t val = (int64_t)(get_bytes(b, index, 4, order_big(args, 1)) & 0xffffffffULL);
    if (inc) b->rcur += 4;
    return &av_int_new(val)->base.base;
}

static av_type *buf_getLong(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getLong", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 8)) return buf_thrown("Buffer.getLong(): Index out of bounds.");
    int64_t val = (int64_t)get_bytes(b, index, 8, order_big(args, 1));
    if (inc) b->rcur += 8;
    return &av_int_new(val)->base.base;
}

static av_type *buf_getFloat(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getFloat", &inc, &index);
    if (bad != NULL) return bad;
    /* Java _getFloat reuses the "getInt" message text. */
    if (!in_bounds(b, index, 4)) return buf_thrown("Buffer.getInt(): Index out of bounds.");
    double val = (double)b2f((uint32_t)get_bytes(b, index, 4, order_big(args, 1)));
    if (inc) b->rcur += 4;
    return &av_double_new(val)->base.base;
}

static av_type *buf_getDouble(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    bool inc;
    int index;
    av_type *bad = read_index(b, args, "getDouble", &inc, &index);
    if (bad != NULL) return bad;
    if (!in_bounds(b, index, 8)) return buf_thrown("Buffer.getDouble(): Index out of bounds.");
    double val = b2d(get_bytes(b, index, 8, order_big(args, 1)));
    if (inc) b->rcur += 4;   /* Java advances 4 here, not 8 (reproduced) */
    return &av_double_new(val)->base.base;
}

/* --- string access ----------------------------------------------- */

static av_type *buf_addString(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    size_t slen;
    const uint16_t *str = str_units(args, 0, &slen);
    if (str == NULL) return buf_exc("buf_addString: argument 1 must be a string.");
    const char *cset = opt_str(args, 1, "utf_8");
    /* _setStringAt: Charset.forName throws for an unknown charset. */
    const cs_charset *cs;
    av_type *bad = charset_forname(cset, &cs);
    if (bad != NULL) return bad;
    size_t elen;
    unsigned char *enc = cs_encode_units(cs, str, slen, &elen);
    /* Java's getBytes cannot return an array this long, and narrowing
     * it to int would make it negative. */
    if (elen > INT_MAX) return stdlib_array_too_large();
    int n = (int)elen;
    /* _setStringAt at the write cursor, with buf_setStringAt's checks:
     * past the end is its own thrown message (computed in 64 bits so it
     * cannot wrap), and a negative cursor is
     * System.arraycopy's throw. The cursor can be negative because the
     * advance below wraps. */
    if ((int64_t)b->wcur + n > b->size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Buffer.setStringAt(): Index %lld/%d out of bounds.",
                 (long long)b->wcur + n, b->size);
        return buf_thrown(msg);
    }
    av_type *ac = arraycopy_check(n, 0, b->size, b->wcur, n);
    if (ac != NULL) return ac;
    memcpy(b->data + b->wcur, enc, (size_t)n);
    /* Java advances by str.getBytes().length, the UTF-8 length,
     * whatever charset was written. That can be up to three times n,
     * so the cursor can pass INT_MAX; it wraps as Java's int += does,
     * done in unsigned arithmetic so it is defined in C. */
    size_t u8len;
    cs_encode_units(cs_lookup("UTF-8"), str, slen, &u8len);
    b->wcur = (int32_t)((uint32_t)b->wcur + (uint32_t)u8len);
    return self;
}

static av_type *buf_getString(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    const char *gcs = opt_str(args, 0, "utf_8");
    const cs_charset *cs = charset_for(gcs);
    if (cs == NULL) {
        char m[160];
        snprintf(m, sizeof(m), "Buffer.getString(): Unsopported encoding exception. (%s)", cset_name(gcs));
        return buf_thrown(m);
    }
    size_t un;
    uint16_t *u = cs_decode_units(cs, b->data, (size_t)b->size, &un);
    return &av_string_new_units(u, un)->base.base;
}

static av_type *buf_getStringAt(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    /* ABuffer.getStringAt checks the 64-bit values in this order: any
     * negative index reads at the cursor, then the length, the index,
     * and the end of the read; the charset only when reading. */
    int64_t raw_len = req_int(args, 0);
    av_type *iv = av_list_size(args) > 1 ? av_list_get(args, 1) : NULL;
    int64_t raw_idx = (iv != NULL && iv->type != C_TYPE_NULL) ? av_numeric_int(iv) : -1;
    bool inc = false;
    if (raw_idx < 0) { raw_idx = b->rcur; inc = true; }
    av_type *bad = check_size("getStringAt", raw_len);
    if (bad != NULL) return bad;
    char msg[192];
    if (raw_idx > INT_MAX) {
        snprintf(msg, sizeof(msg), "Buffer.getStringAt(): Index of %lld is out of range "
                 "for a buffer of %d bytes.", (long long)raw_idx, b->size);
        return buf_exc(msg);
    }
    int index = (int)raw_idx;
    int length = (int)raw_len;
    if ((int64_t)index + length > b->size) {
        snprintf(msg, sizeof(msg), "Buffer.getStringAt(): Reading %d bytes at index %d runs "
                 "past the end of a buffer of %d bytes.", length, index, b->size);
        return buf_exc(msg);
    }
    const char *cset = opt_str(args, 2, "utf_8");
    const cs_charset *cs = charset_for(cset);
    if (cs == NULL) {
        snprintf(msg, sizeof(msg), "Buffer.getStringAt(): Unsopported encoding exception. (%s)", cset_name(cset));
        return buf_thrown(msg);
    }
    size_t un;
    uint16_t *u = cs_decode_units(cs, b->data + index, (size_t)length, &un);
    if (inc) b->rcur += length;
    return &av_string_new_units(u, un)->base.base;
}

static av_type *buf_setString(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    size_t slen;
    const uint16_t *str = str_units(args, 0, &slen);
    if (str == NULL) return buf_exc("buf_setString: argument 1 must be a string.");
    const char *scs = opt_str(args, 1, "utf_8");
    const cs_charset *cs = charset_for(scs);
    if (cs == NULL) {
        char m[160];
        snprintf(m, sizeof(m), "Buffer._setString(): Unsupported encoding exception. %s", cset_name(scs));
        return buf_thrown(m);
    }
    size_t elen;
    unsigned char *nd = cs_encode_units(cs, str, slen, &elen);
    /* Java's getBytes cannot return an array this long, and narrowing
     * it to int would make it negative. */
    if (elen > INT_MAX) return stdlib_array_too_large();
    int n = (int)elen;
    b->data = nd;        /* replaces the whole buffer, per Java */
    b->size = n;
    return &av_int_new(n)->base.base;
}

static av_type *buf_setStringAt(av_type *self, environment *env, av_list *args)
{
    (void)env;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    int index;
    av_type *bad = req_index("setStringAt", args, 0, &index);
    if (bad != NULL) return bad;
    size_t slen;
    const uint16_t *str = str_units(args, 1, &slen);
    if (str == NULL) return buf_exc("buf_setStringAt: argument 2 must be a string.");
    const char *tcs = opt_str(args, 2, "utf_8");
    const cs_charset *cs;
    bad = charset_forname(tcs, &cs);
    if (bad != NULL) return bad;
    size_t elen;
    unsigned char *enc = cs_encode_units(cs, str, slen, &elen);
    /* Java's getBytes cannot return an array this long, and narrowing
     * it to int would make it negative. */
    if (elen > INT_MAX) return stdlib_array_too_large();
    int n = (int)elen;
    /* _setStringAt: past the end is its own thrown message (index plus
     * length, computed in 64 bits so it cannot wrap); a
     * negative index that stays inside is System.arraycopy's throw. */
    if ((int64_t)index + n > b->size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Buffer.setStringAt(): Index %lld/%d out of bounds.",
                 (long long)index + n, b->size);
        return buf_thrown(msg);
    }
    av_type *ac = arraycopy_check(n, 0, b->size, index, n);
    if (ac != NULL) return ac;
    memcpy(b->data + index, enc, (size_t)n);
    return &av_int_new(n)->base.base;
}

/* --- copy -------------------------------------------------------- */

static av_type *buf_copyFrom(av_type *self, environment *env, av_list *args)
{
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    /* ABuffer.copyFrom: all three ints are narrowed first, then the
     * other object is checked. Java's own bounds tests are strict and
     * computed in int; the sums are taken in 64 bits here so they
     * cannot wrap. Whatever gets past them is
     * System.arraycopy's to check. */
    int dindex, sindex, length;
    av_type *bad = req_index("copyFrom", args, 0, &dindex);
    if (bad != NULL) return bad;
    bad = opt_index("copyFrom", args, 2, -1, &sindex);
    if (bad != NULL) return bad;
    bad = opt_index("copyFrom", args, 3, -1, &length);
    if (bad != NULL) return bad;
    buf_state *src = av_buffer_state(env, av_list_get(args, 1));
    if (src == NULL) return buf_exc("Buffer.copyFrom(): External object is null or not of type Buffer.");
    if (!((int64_t)dindex + length < b->size)) return buf_exc("Buffer.copyFrom(): Destination buffer overflow.");
    if (!((int64_t)sindex + length < src->size)) return buf_exc("Buffer.copyFrom(): Source buffer overflow.");
    av_type *ac = arraycopy_check(src->size, sindex, b->size, dindex, length);
    if (ac != NULL) return ac;
    memmove(b->data + dindex, src->data + sindex, (size_t)length);
    return self;
}

static av_type *buf_copyTo(av_type *self, environment *env, av_list *args)
{
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    /* ABuffer.copyTo: as copyFrom, and Java tests the destination
     * index against this buffer and the source index against the
     * other one; System.arraycopy then checks the real bounds. */
    int sindex, dindex, length;
    av_type *bad = req_index("copyTo", args, 0, &sindex);
    if (bad != NULL) return bad;
    bad = opt_index("copyTo", args, 2, -1, &dindex);
    if (bad != NULL) return bad;
    bad = opt_index("copyTo", args, 3, -1, &length);
    if (bad != NULL) return bad;
    buf_state *dst = av_buffer_state(env, av_list_get(args, 1));
    if (dst == NULL) return buf_exc("Buffer.copyTo(): External object is null or not of type Buffer.");
    if (!((int64_t)dindex + length < b->size)) return buf_exc("Buffer.copyTo(): Destination buffer overflow.");
    if (!((int64_t)sindex + length < dst->size)) return buf_exc("Buffer.copyTo(): Source buffer overflow.");
    av_type *ac = arraycopy_check(b->size, sindex, dst->size, dindex, length);
    if (ac != NULL) return ac;
    memmove(dst->data + dindex, b->data + sindex, (size_t)length);
    return self;
}

/* --- binary representation --------------------------------------- */

static void byte_bin(unsigned char by, char out[9])
{
    for (int i = 0; i < 8; i++) out[i] = (by & (0x80 >> i)) ? '1' : '0';
    out[8] = '\0';
}

static av_type *to_binary(av_type *self, environment *env, av_list *args, int width,
                          const char *method, const char *oob_msg)
{
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    int index;
    av_type *bad = req_index(method, args, 0, &index);
    if (bad != NULL) return bad;
    bool big = width == 1 ? true : order_big(args, 1);
    /* ABuffer's xToBinary lets the index run width - 1 past the end
     * (index < length + width - 1); a read past the array then throws
     * ArrayIndexOutOfBoundsException at the first byte outside it. */
    if (!(index >= 0 && (int64_t)index < (int64_t)b->size + width - 1)) return buf_exc(oob_msg);
    if ((int64_t)index + width > b->size) {
        char m[128];
        int first = index > b->size ? index : b->size;
        snprintf(m, sizeof(m), "java.lang.ArrayIndexOutOfBoundsException: Index %d out of "
                 "bounds for length %d", first, b->size);
        return &av_exception_new_thrown(m)->base.base;
    }
    char out[65];
    out[0] = '\0';
    if (big) {
        for (int i = 0; i < width; i++) { char s[9]; byte_bin(b->data[index + i], s); memcpy(out + i * 8, s, 8); }
    } else {
        for (int i = 0; i < width; i++) { char s[9]; byte_bin(b->data[index + (width - 1 - i)], s); memcpy(out + i * 8, s, 8); }
    }
    out[width * 8] = '\0';
    return &av_string_new(out)->base.base;
}

static av_type *buf_byteToBinary(av_type *self, environment *env, av_list *args)
{ (void)env; return to_binary(self, env, args, 1, "byteToBinary", "Buffer.byteToBinary(): Index out of bounds."); }
static av_type *buf_shortToBinary(av_type *self, environment *env, av_list *args)
{ (void)env; return to_binary(self, env, args, 2, "shortToBinary", "Buffer.shortToBinary(): Index out of bounds."); }
static av_type *buf_intToBinary(av_type *self, environment *env, av_list *args)
{ (void)env; return to_binary(self, env, args, 4, "intToBinary", "Buffer.intToBinary(): Index out of bounds."); }
static av_type *buf_longToBinary(av_type *self, environment *env, av_list *args)
{ (void)env; return to_binary(self, env, args, 8, "longToBinary", "Buffer.longToBinary(): Index out of bounds."); }
static av_type *buf_floatToBinary(av_type *self, environment *env, av_list *args)
{ (void)env; return to_binary(self, env, args, 4, "floatToBinary", "Buffer.intToBinary(): Index out of bounds."); }
static av_type *buf_doubleToBinary(av_type *self, environment *env, av_list *args)
{ (void)env; return to_binary(self, env, args, 8, "doubleToBinary", "Buffer.doubleToBinary(): Index out of bounds."); }

/* --- serialization (toJson / pack) ------------------------------- */

/* ABase64.encode(bytes) == hex(base64(bytes)). */
static char *buffer_b64hex(buf_state *b)
{
    char *b64 = codec_base64_encode(b->data, (size_t)b->size);
    return codec_hex_encode((const unsigned char *)b64, strlen(b64));
}

static av_type *buf_toJson(av_type *self, environment *env, av_list *args)
{
    (void)args;
    buf_state *b = self_buf(self, env);
    if (b == NULL) return buf_bad_self();
    char *enc = buffer_b64hex(b);
    size_t n = strlen(enc);
    char *out = AUSSOM_ALLOC_ATOMIC(n + 3);
    out[0] = '"';
    memcpy(out + 1, enc, n);
    out[n + 1] = '"';
    out[n + 2] = '\0';
    return &av_string_new(out)->base.base;
}

/* --- shared helpers for the codec classes ------------------------ */

const char *av_buffer_encoded_str(av_type *buf_obj)
{
    /* Env-less path (av_json pack dispatches by class name). Validate
     * via the magic tag so a redefined or confused "Buffer" cannot have
     * its object header read as a buf_state. Returns NULL
     * when the receiver is not a genuine Buffer; the caller falls back
     * to generic object serialization. */
    if (buf_obj == NULL || buf_obj->type != C_TYPE_OBJECT) return NULL;
    void *eo = av_object_get_extern_object((av_object *)buf_obj);
    if (eo == NULL || eo == (void *)buf_obj ||
        ((buf_state *)eo)->magic != BUF_MAGIC)
        return NULL;
    return buffer_b64hex((buf_state *)eo);
}

buf_state *av_buffer_state(environment *env, av_type *obj)
{
    (void)env;
    return buffer_of(obj);
}

av_type *stdlib_array_too_large(void)
{
    return &av_exception_new_thrown("java.lang.OutOfMemoryError: Requested "
                                    "array size exceeds VM limit")->base.base;
}

av_type *av_buffer_create(environment *env, const unsigned char *bytes, int len)
{
    if (len < 0) return stdlib_array_too_large();
    ast_class *cls = engine_resolve_class(environment_engine(env), "Buffer");
    if (cls == NULL) return buf_exc("Buffer class not found.");
    av_type *inst = class_instantiate(env, cls, false, av_list_new());
    if (av_is_ex(inst)) return inst;
    buf_state *b = av_buffer_state(env, inst);
    if (b == NULL) return buf_exc("Buffer class not found.");
    unsigned char *nd = AUSSOM_ALLOC_ATOMIC(len > 0 ? (size_t)len : 1);
    if (len > 0) memcpy(nd, bytes, (size_t)len);
    b->data = nd;
    b->size = len;
    b->wcur = 0;
    b->rcur = 0;
    return inst;
}

const aussom_extern_method stdlib_abuffer_methods[] = {
    { "newBuffer", buf_newBuffer },
    { "size", buf_size },
    { "clear", buf_clear },
    { "writeSeek", buf_writeSeek },
    { "readSeek", buf_readSeek },
    { "getWriteCursor", buf_getWriteCursor },
    { "getReadCursor", buf_getReadCursor },
    { "addString", buf_addString },
    { "addByte", buf_addByte },
    { "addUByte", buf_addUByte },
    { "addShort", buf_addShort },
    { "addUShort", buf_addUShort },
    { "addInt", buf_addInt },
    { "addUInt", buf_addUInt },
    { "addLong", buf_addLong },
    { "addFloat", buf_addFloat },
    { "addDouble", buf_addDouble },
    { "getString", buf_getString },
    { "getStringAt", buf_getStringAt },
    { "getByte", buf_getByte },
    { "getUByte", buf_getUByte },
    { "getShort", buf_getShort },
    { "getUShort", buf_getUShort },
    { "getInt", buf_getInt },
    { "getUInt", buf_getUInt },
    { "getLong", buf_getLong },
    { "getFloat", buf_getFloat },
    { "getDouble", buf_getDouble },
    { "setString", buf_setString },
    { "setStringAt", buf_setStringAt },
    { "setByte", buf_setByte },
    { "setUByte", buf_setUByte },
    { "setShort", buf_setShort },
    { "setUShort", buf_setUShort },
    { "setInt", buf_setInt },
    { "setUInt", buf_setUInt },
    { "setLong", buf_setLong },
    { "setFloat", buf_setFloat },
    { "setDouble", buf_setDouble },
    { "copyFrom", buf_copyFrom },
    { "copyTo", buf_copyTo },
    { "byteToBinary", buf_byteToBinary },
    { "shortToBinary", buf_shortToBinary },
    { "intToBinary", buf_intToBinary },
    { "longToBinary", buf_longToBinary },
    { "floatToBinary", buf_floatToBinary },
    { "doubleToBinary", buf_doubleToBinary },
    { "toJson", buf_toJson },
    { "pack", stdlib_pack },
    { NULL, NULL }
};
