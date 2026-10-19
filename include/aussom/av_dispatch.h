/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Internal dispatch header for the runtime value system. Each
 * concrete type implements:
 *
 *   void av_<t>_write(const av_type *v, int level, FILE *out);      // toString(level)
 *   void av_<t>_write_str(const av_type *v, int level, FILE *out);  // str(level)
 *   av_type *av_<t>_clone(const av_type *v);
 *
 * av_type.c switches on the c_type tag to reach these. Not part of
 * the public API.
 */

#ifndef AUSSOM_AV_DISPATCH_H
#define AUSSOM_AV_DISPATCH_H

#include <stdio.h>

#include "aussom/av_type.h"

void av_null_write(const av_type *v, int level, FILE *out);
void av_null_write_str(const av_type *v, int level, FILE *out);
av_type *av_null_clone(const av_type *v);

void av_bool_write(const av_type *v, int level, FILE *out);
void av_bool_write_str(const av_type *v, int level, FILE *out);
av_type *av_bool_clone(const av_type *v);

void av_int_write(const av_type *v, int level, FILE *out);
void av_int_write_str(const av_type *v, int level, FILE *out);
av_type *av_int_clone(const av_type *v);

void av_double_write(const av_type *v, int level, FILE *out);
void av_double_write_str(const av_type *v, int level, FILE *out);
av_type *av_double_clone(const av_type *v);

void av_string_write(const av_type *v, int level, FILE *out);
void av_string_write_str(const av_type *v, int level, FILE *out);
av_type *av_string_clone(const av_type *v);

void av_list_write(const av_type *v, int level, FILE *out);
void av_list_write_str(const av_type *v, int level, FILE *out);
av_type *av_list_clone(const av_type *v);

void av_map_write(const av_type *v, int level, FILE *out);
void av_map_write_str(const av_type *v, int level, FILE *out);
av_type *av_map_clone(const av_type *v);

void av_object_write(const av_type *v, int level, FILE *out);
void av_object_write_str(const av_type *v, int level, FILE *out);
av_type *av_object_clone(const av_type *v);

void av_callback_write(const av_type *v, int level, FILE *out);
void av_callback_write_str(const av_type *v, int level, FILE *out);
av_type *av_callback_clone(const av_type *v);

void av_return_write(const av_type *v, int level, FILE *out);
void av_return_write_str(const av_type *v, int level, FILE *out);
av_type *av_return_clone(const av_type *v);

void av_break_write(const av_type *v, int level, FILE *out);
void av_break_write_str(const av_type *v, int level, FILE *out);
av_type *av_break_clone(const av_type *v);

void av_ref_write(const av_type *v, int level, FILE *out);
void av_ref_write_str(const av_type *v, int level, FILE *out);
av_type *av_ref_clone(const av_type *v);

void av_exception_write(const av_type *v, int level, FILE *out);
void av_exception_write_str(const av_type *v, int level, FILE *out);
av_type *av_exception_clone(const av_type *v);

#endif /* AUSSOM_AV_DISPATCH_H */
