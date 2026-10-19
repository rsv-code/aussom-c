/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "aussom/ast_node_type.h"

const char *ast_node_type_name(ast_node_type t)
{
    switch (t) {
    case AST_TYPE_UNDEF:             return "UNDEF";
    case AST_TYPE_NULL:              return "NULL";
    case AST_TYPE_BOOL:              return "BOOL";
    case AST_TYPE_INT:               return "INT";
    case AST_TYPE_DOUBLE:            return "DOUBLE";
    case AST_TYPE_STRING:            return "STRING";
    case AST_TYPE_LIST:              return "LIST";
    case AST_TYPE_MAP:               return "MAP";
    case AST_TYPE_INCLUDE:           return "INCLUDE";
    case AST_TYPE_CLASS:             return "CLASS";
    case AST_TYPE_OBJ:               return "OBJ";
    case AST_TYPE_VAR:               return "VAR";
    case AST_TYPE_FUNCTDEF:          return "FUNCTDEF";
    case AST_TYPE_FUNCTDEFARGSLIST:  return "FUNCTDEFARGSLIST";
    case AST_TYPE_STATEMENTLIST:     return "STATEMENTLIST";
    case AST_TYPE_EXP:               return "EXP";
    case AST_TYPE_FUNCTCALL:         return "FUNCTCALL";
    case AST_TYPE_RETURN:            return "RETURN";
    case AST_TYPE_TRYCATCH:          return "TRYCATCH";
    case AST_TYPE_THROW:             return "THROW";
    case AST_TYPE_NEWINST:           return "NEWINST";
    case AST_TYPE_IFELSE:            return "IFELSE";
    case AST_TYPE_CONDITION:         return "CONDITION";
    case AST_TYPE_SWITCH:            return "SWITCH";
    case AST_TYPE_WHILE:             return "WHILE";
    case AST_TYPE_FOR:               return "FOR";
    case AST_TYPE_BREAK:             return "BREAK";
    case AST_TYPE_ETCETERA:          return "ETCETERA";
    case AST_TYPE_CALLBACK:          return "CALLBACK";
    case AST_TYPE_AUSSOM_DOC:        return "AUSSOM_DOC";
    case AST_TYPE_ANNOTATION:        return "ANNOTATION";
    case AST_TYPE_ANNOTATION_ARG:    return "ANNOTATION_ARG";
    }
    return "UNDEF";
}
