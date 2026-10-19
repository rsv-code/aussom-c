/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Internal header: declares the per-subtype write_body helpers that
 * the base dispatcher in ast_node.c calls. Each subtype
 * implementation provides:
 *
 *   void ast_<name>_write_body(const ast_node *n, int level, FILE *out);
 *
 * write_body emits the full subtype-specific toString output between
 * the outer braces. The dispatcher does not call any free helper
 * because memory is GC-managed (see aussom_gc.h); the corresponding
 * Java destructor logic has no C equivalent here.
 *
 * Not part of the public API; included by the dispatcher and by each
 * subtype .c file.
 */

#ifndef AUSSOM_AST_DISPATCH_H
#define AUSSOM_AST_DISPATCH_H

#include <stdio.h>

#include "aussom/ast_node.h"

void ast_null_write_body(const ast_node *n, int level, FILE *out);
void ast_bool_write_body(const ast_node *n, int level, FILE *out);
void ast_int_write_body(const ast_node *n, int level, FILE *out);
void ast_double_write_body(const ast_node *n, int level, FILE *out);
void ast_string_write_body(const ast_node *n, int level, FILE *out);
void ast_list_write_body(const ast_node *n, int level, FILE *out);
void ast_map_write_body(const ast_node *n, int level, FILE *out);
void ast_etcetera_write_body(const ast_node *n, int level, FILE *out);
void ast_var_write_body(const ast_node *n, int level, FILE *out);
void ast_obj_write_body(const ast_node *n, int level, FILE *out);
void ast_statement_list_write_body(const ast_node *n, int level, FILE *out);
void ast_expression_write_body(const ast_node *n, int level, FILE *out);
void ast_funct_call_write_body(const ast_node *n, int level, FILE *out);
void ast_funct_def_write_body(const ast_node *n, int level, FILE *out);
void ast_funct_def_args_list_write_body(const ast_node *n, int level, FILE *out);
void ast_class_write_body(const ast_node *n, int level, FILE *out);
void ast_new_inst_write_body(const ast_node *n, int level, FILE *out);
void ast_callback_write_body(const ast_node *n, int level, FILE *out);
void ast_include_write_body(const ast_node *n, int level, FILE *out);
void ast_ifelse_write_body(const ast_node *n, int level, FILE *out);
void ast_condition_block_write_body(const ast_node *n, int level, FILE *out);
void ast_switch_write_body(const ast_node *n, int level, FILE *out);
void ast_while_write_body(const ast_node *n, int level, FILE *out);
void ast_for_write_body(const ast_node *n, int level, FILE *out);
void ast_break_write_body(const ast_node *n, int level, FILE *out);
void ast_return_write_body(const ast_node *n, int level, FILE *out);
void ast_trycatch_write_body(const ast_node *n, int level, FILE *out);
void ast_throw_write_body(const ast_node *n, int level, FILE *out);
void ast_annotation_write_body(const ast_node *n, int level, FILE *out);
void ast_annotation_arg_write_body(const ast_node *n, int level, FILE *out);
void ast_aussom_doc_write_body(const ast_node *n, int level, FILE *out);

#endif /* AUSSOM_AST_DISPATCH_H */
