/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Umbrella header: includes every public AST type. New code should
 * prefer including the specific subtype header that it needs, but
 * this header is convenient for top-level driver code and tests.
 */

#ifndef AUSSOM_AST_H
#define AUSSOM_AST_H

#include "aussom/access_type.h"
#include "aussom/aussom_exception.h"
#include "aussom/c_type.h"
#include "aussom/exp_type.h"

#include "aussom/ast_node.h"
#include "aussom/ast_node_type.h"

#include "aussom/ast_annotation.h"
#include "aussom/ast_annotation_arg.h"
#include "aussom/ast_aussom_doc.h"
#include "aussom/ast_bool.h"
#include "aussom/ast_break.h"
#include "aussom/ast_callback.h"
#include "aussom/ast_class.h"
#include "aussom/ast_condition_block.h"
#include "aussom/ast_double.h"
#include "aussom/ast_etcetera.h"
#include "aussom/ast_expression.h"
#include "aussom/ast_for.h"
#include "aussom/ast_funct_call.h"
#include "aussom/ast_funct_def.h"
#include "aussom/ast_funct_def_args_list.h"
#include "aussom/ast_ifelse.h"
#include "aussom/ast_include.h"
#include "aussom/ast_int.h"
#include "aussom/ast_list.h"
#include "aussom/ast_map.h"
#include "aussom/ast_new_inst.h"
#include "aussom/ast_null.h"
#include "aussom/ast_obj.h"
#include "aussom/ast_return.h"
#include "aussom/ast_statement_list.h"
#include "aussom/ast_string.h"
#include "aussom/ast_switch.h"
#include "aussom/ast_throw.h"
#include "aussom/ast_trycatch.h"
#include "aussom/ast_var.h"
#include "aussom/ast_while.h"

#include "aussom/doc/doc_text.h"
#include "aussom/doc/doc_type.h"

#endif /* AUSSOM_AST_H */
