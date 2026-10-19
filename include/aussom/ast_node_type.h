/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AUSSOM_AST_NODE_TYPE_H
#define AUSSOM_AST_NODE_TYPE_H

/* Mirrors com.aussom.ast.astNodeType. The dispatcher in ast_node.c
 * switches on this tag to pick the per-type implementation. */
typedef enum ast_node_type {
    AST_TYPE_UNDEF = 0,
    AST_TYPE_NULL,
    AST_TYPE_BOOL,
    AST_TYPE_INT,
    AST_TYPE_DOUBLE,
    AST_TYPE_STRING,
    AST_TYPE_LIST,
    AST_TYPE_MAP,
    AST_TYPE_INCLUDE,
    AST_TYPE_CLASS,
    AST_TYPE_OBJ,
    AST_TYPE_VAR,
    AST_TYPE_FUNCTDEF,
    AST_TYPE_FUNCTDEFARGSLIST,
    AST_TYPE_STATEMENTLIST,
    AST_TYPE_EXP,
    AST_TYPE_FUNCTCALL,
    AST_TYPE_RETURN,
    AST_TYPE_TRYCATCH,
    AST_TYPE_THROW,
    AST_TYPE_NEWINST,
    AST_TYPE_IFELSE,
    AST_TYPE_CONDITION,
    AST_TYPE_SWITCH,
    AST_TYPE_WHILE,
    AST_TYPE_FOR,
    AST_TYPE_BREAK,
    AST_TYPE_ETCETERA,
    AST_TYPE_CALLBACK,
    AST_TYPE_AUSSOM_DOC,
    /* C-port-only tags: the Java side keeps the base type at UNDEF
     * for annotation/annotation_arg and relies on virtual dispatch.
     * The C dispatcher needs explicit tags to free subtype fields.
     * These never appear in the JSON-style toString output because
     * the corresponding write_body functions do not emit the
     * common type header. */
    AST_TYPE_ANNOTATION,
    AST_TYPE_ANNOTATION_ARG
} ast_node_type;

/* Returns the printable name used in JSON-style toString output. The
 * names mirror the Java enum names exactly so dumped trees diff cleanly
 * against aussom-base output. */
const char *ast_node_type_name(ast_node_type t);

#endif /* AUSSOM_AST_NODE_TYPE_H */
