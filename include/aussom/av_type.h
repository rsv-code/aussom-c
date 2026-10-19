/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Runtime value type system ("av" = Aussom Value). Mirrors
 * com.aussom.types. The base AussomType becomes av_type; the
 * AussomObject layer becomes av_object; concrete values (null, bool,
 * int, double, string, list, map) embed av_object, while the control
 * carriers (return, break, ref) embed av_type directly. All values
 * are GC-allocated; never freed by hand.
 *
 * The "av_" prefix keeps this layer distinct from the AST layer
 * (ast_*) and the engine-level helpers (aussom_gc, aussom_exception),
 * notably avoiding a name clash with the engine-level aussomException.
 */

#ifndef AUSSOM_AV_TYPE_H
#define AUSSOM_AV_TYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "aussom/c_type.h"

/* Universal base. Every runtime value embeds this as its first
 * member, so a `subtype *` casts to `av_type *` and back. */
typedef struct av_type {
    /* Aligned to the strictest member any subtype holds, rather than to
     * this one field.
     *
     * Every value in the hierarchy embeds av_type at offset zero and is
     * allocated as its concrete subtype, so a downcast from av_type *
     * always yields a correctly aligned pointer. Declaring that here
     * states the invariant to the compiler: without it, av_type aligns
     * to 4 while the subtypes align to 8, and every one of the ~270
     * tagged downcasts across the interpreter draws a -Wcast-align
     * warning on a strict-alignment target such as arm64. The
     * alternative would have been to launder each of those casts
     * through void *, which hides real alignment bugs as readily as
     * false ones.
     *
     * int64_t is the strictest member in the family (av_int), and it
     * requires 8-byte alignment on 32-bit ARM as well as 64-bit, so it
     * is the right thing to name. This costs nothing: av_object already
     * pads to 8 for its pointers. */
    _Alignas(int64_t) c_type type;
    /* Java's System.identityHashCode for this value: 0 until
     * av_identity first asks for it. It sits in what would otherwise
     * be padding before the next 8-byte field, so it costs no space. */
    uint32_t identity;
} av_type;

/* --- type predicates (mirror AussomType.isX) --------------------- */

/* True for a cException that is not flagged as a local (caught)
 * object -- i.e. an exception currently propagating. */
bool av_is_ex(const av_type *v);
bool av_is_null(const av_type *v);
bool av_is_return(const av_type *v);
bool av_is_break(const av_type *v);
bool av_is_numeric(const av_type *v);

/* --- numeric coercion (mirror AussomType.getNumericX) ------------
 * Defined for cBool / cInt / cDouble. For any other tag they return
 * a zero value; callers gate with av_is_numeric where the Java code
 * would have thrown. */
bool   av_numeric_bool(const av_type *v);
int64_t av_numeric_int(const av_type *v);
double av_numeric_double(const av_type *v);

/* A number that names this value for display, like Java's
 * System.identityHashCode: stable for the value's life, never 0, and
 * not derived from its address, so printing it reveals nothing about
 * the memory layout. Assigned on first call. */
uint32_t av_identity(av_type *v);

/* Java's (long) cast of a double: NaN becomes 0, and a value past
 * either end of the int64 range becomes that end. A plain C cast is
 * undefined for these, and x86-64 and ARM64 give different results. */
int64_t av_double_to_int(double d);

/* --- comparison / truthiness ------------------------------------- */

/* Structural equality, mirroring AussomType.compareEqual. Returns an
 * av_bool* on success or an av_exception* when the two types cannot
 * be compared. The result is always a fresh value. */
av_type *av_compare_equal(const av_type *a, const av_type *b);

/* Collection-membership equality (= the AussomType.equals overrides).
 * Distinct from av_compare_equal, which implements the `==` OPERATOR
 * and therefore compares across the numeric types. Here a value matches
 * only within its own type: null/bool/int/double/string by value, a
 * callback by bound object plus function name, and objects, lists and
 * maps by identity. Used by list contains/indexOf/remove/removeAll/
 * retainAll and map containsVal. */
bool av_value_equals(const av_type *a, const av_type *b);

/* Truthiness for use as a condition, mirroring
 * AussomType.evalExpressionBool. Returns an av_bool* or, for an
 * unhandled type, an av_exception*. */
av_type *av_eval_expression_bool(const av_type *v);

/* --- string forms ------------------------------------------------ */

/* str(): the human form used for concatenation and printing.
 * Returns a GC-managed string. */
char *av_str(const av_type *v);

/* str(level): the indented form some container types use. */
char *av_str_level(const av_type *v, int level);

/* Writes the str(level) form to a stream (used by container types to
 * build their children without quadratic concatenation). */
void av_write_str(const av_type *v, int level, FILE *out);

/* toString(level): the JSON-style structural dump. */
void av_to_string(const av_type *v, int level, FILE *out);
char *av_to_string_alloc(const av_type *v);

/* --- clone ------------------------------------------------------- */

/* Deep clone mirroring AussomClonable.clone(). Scalars copy their
 * value; containers clone their elements. */
av_type *av_clone(const av_type *v);

/* Writes `level` tab characters; shared by every type's dump. */
void av_write_tabs(int level, FILE *out);

#endif /* AUSSOM_AV_TYPE_H */
