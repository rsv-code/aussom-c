/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Internal: the stdlib's extern method tables, one per extern class,
 * gathered into the built-in packages by stdlib_packages.c.
 */

#ifndef AUSSOM_STDLIB_INTERNAL_H
#define AUSSOM_STDLIB_INTERNAL_H

#include <stdint.h>

#include "aussom/av_json.h"
#include "aussom/av_list.h"
#include "aussom/av_type.h"
#include "aussom/environment.h"
#include "aussom/extern_registry.h"

/* Package com.aussom.types. */
extern const aussom_extern_method stdlib_aussom_bool_methods[];
extern const aussom_extern_method stdlib_aussom_int_methods[];
extern const aussom_extern_method stdlib_aussom_double_methods[];
extern const aussom_extern_method stdlib_aussom_string_methods[];
extern const aussom_extern_method stdlib_aussom_list_methods[];
extern const aussom_extern_method stdlib_aussom_map_methods[];
extern const aussom_extern_method stdlib_aussom_object_methods[];
extern const aussom_extern_method stdlib_aussom_callback_methods[];
extern const aussom_extern_method stdlib_aussom_null_methods[];
extern const aussom_extern_method stdlib_aussom_exception_methods[];

/* Package com.aussom.stdlib. ATest's and ATestRunner's
 * tables are declared in test_runner.h. */
extern const aussom_extern_method stdlib_sbool_methods[];
extern const aussom_extern_method stdlib_sint_methods[];
extern const aussom_extern_method stdlib_sdouble_methods[];
extern const aussom_extern_method stdlib_console_methods[];
extern const aussom_extern_method stdlib_adate_methods[];
extern const aussom_extern_method stdlib_abuffer_methods[];
extern const aussom_extern_method stdlib_alang_methods[];
extern const aussom_extern_method stdlib_ajson_methods[];
extern const aussom_extern_method stdlib_asecman_methods[];
extern const aussom_extern_method stdlib_asecurity_manager_methods[];
extern const aussom_extern_method stdlib_aatomic_long_methods[];
extern const aussom_extern_method stdlib_aatomic_bool_methods[];
extern const aussom_extern_method stdlib_aatomic_ref_methods[];
extern const aussom_extern_method stdlib_acounter_methods[];
extern const aussom_extern_method stdlib_ablocking_queue_methods[];
extern const aussom_extern_method stdlib_asemaphore_methods[];
extern const aussom_extern_method stdlib_alatch_methods[];
extern const aussom_extern_method stdlib_alock_methods[];
extern const aussom_extern_method stdlib_amath_methods[];
extern const aussom_extern_method stdlib_asys_methods[];
extern const aussom_extern_method stdlib_abase64_methods[];
extern const aussom_extern_method stdlib_ahex_methods[];
extern const aussom_extern_method stdlib_auuid_methods[];
extern const aussom_extern_method stdlib_aregex_methods[];
extern const aussom_extern_method stdlib_areflect_methods[];
extern const aussom_extern_method stdlib_aclass_methods[];

/* Util.checkIntRange: checks that `value` fits a Java int before it is
 * narrowed to one. Returns the exception, naming `method` (such as
 * "list.get"), if it does not fit, otherwise NULL. */
av_type *stdlib_check_int_range(const char *method, int64_t value);

/* PCRE2-backed regex helpers shared by the `regex` class and the
 * string regex methods (matches / replaceRegex / replaceFirstRegex /
 * split).
 *
 * All take the environment so they can read the engine's regex step
 * budget (aussom.limit.regex.steps) and check the control state
 * between matches. Passing NULL runs unbudgeted. */
av_type *aussom_regex_substitute(environment *env, const char *re,
                                 const char *repl, const char *hay, bool global,
                                 const char **repl_err_class);
av_type *aussom_regex_matches(environment *env, const char *re, const char *hay);

/* Splits `hay` on the regex `re`. On success returns NULL and stores
 * the pieces in *out_parts; on failure returns the exception and
 * leaves *out_parts NULL. Trailing empty pieces are dropped, matching
 * Java's Pattern.split. */
av_type *aussom_regex_split(environment *env, const char *re, const char *hay,
                            av_list **out_parts);

/* Recursively serializes a value to the Aussom "pack" format (mirrors
 * the per-type pack() in the Java runtime: {"type":..,"value"/"members"}).
 * Returns an av_exception on a non-packable type. */
av_type *av_pack(av_type *v);

/* JSONValue.escape of `s`, without quotes. */
void av_json_escape(FILE *out, const char *s);

/* The pack extern, listed in the table of every type that has one. */
av_type *stdlib_pack(av_type *self, environment *env, av_list *args);

/* Date <-> ISO string helpers for json pack / unpack. */
const char *av_date_iso_str(av_type *date_obj);
av_type *av_date_from_iso(environment *env, const char *iso);

/* Buffer encoded value (hex of base64, matching ABase64.encode) used
 * by pack; the caller guarantees `buf_obj` is a Buffer. */
const char *av_buffer_encoded_str(av_type *buf_obj);

/* --- Buffer extern-object state (com.aussom.stdlib.ABuffer) -------
 * A Buffer instance stores this struct as its av_object extern_object
 * (installed by the newBuffer constructor extern). The codec classes
 * and json.unpack read/replace the byte array directly. */
/* First-field tag identifying a genuine buf_state, so the env-less
 * serialization path (av_buffer_encoded_str) and av_buffer_state can
 * reject an object that merely claims to be a Buffer by name but was
 * never initialized as one, or that carries some other extern class's
 * state. Reading it is always in bounds. */
#define BUF_MAGIC 0x42554621u   /* "BUF!" */

typedef struct buf_state {
    uint32_t magic;
    unsigned char *data;   /* GC atomic; `size` bytes */
    int size;
    int wcur;              /* write cursor */
    int rcur;              /* read cursor */
} buf_state;

/* Returns the Buffer state of `obj` (an instance of the Buffer class),
 * or NULL if `obj` is not a Buffer. Mirrors the Java
 * `instanceof ABuffer` check. */
buf_state *av_buffer_state(environment *env, av_type *obj);

/* Instantiates the Buffer class and sets its bytes to a copy of
 * [bytes, bytes+len). Returns the Buffer av_object, or an av_exception
 * (e.g. when the Buffer class is not loaded, or `len` is negative).
 * A caller holding a size_t length checks it against INT_MAX before
 * narrowing (stdlib_array_too_large); a narrowed length that wrapped
 * negative became a negative Buffer size. The length
 * stays an int because host code declares and calls it that way. */
av_type *av_buffer_create(environment *env, const unsigned char *bytes, int len);

/* The error for a byte array longer than a Java array can be: the
 * OutOfMemoryError Java throws when asked for one. */
av_type *stdlib_array_too_large(void);

#endif /* AUSSOM_STDLIB_INTERNAL_H */
