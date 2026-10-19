/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `lang` class (com.aussom.stdlib.ALang) and the methods on
 * the `exception` type (com.aussom.types.AussomException accessors).
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aussom/ast_aussom_doc.h"
#include "aussom/ast_bool.h"
#include "aussom/ast_class.h"
#include "aussom/ast_double.h"
#include "aussom/ast_int.h"
#include "aussom/ast_node_type.h"
#include "aussom/ast_string.h"
#include "aussom/doc/doc_text.h"
#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/av_object.h"
#include "aussom/c_type.h"
#include "aussom/engine.h"
#include "aussom/security_manager.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

/* --- Util.checkIntRange ------------------------------------------ */

av_type *stdlib_check_int_range(const char *method, int64_t value)
{
    char m[192];
    if (value > INT32_MAX) {
        snprintf(m, sizeof(m), "%s(): Value %lld is larger than the maximum int value %d.",
                 method, (long long)value, INT32_MAX);
    } else if (value < INT32_MIN) {
        snprintf(m, sizeof(m), "%s(): Value %lld is smaller than the minimum int value %d.",
                 method, (long long)value, (int)INT32_MIN);
    } else {
        return NULL;
    }
    return &av_exception_new(m)->base.base;
}

/* --- lang -------------------------------------------------------- */

/* lang.type(value): for an object, its class name; otherwise the
 * lowercased c_type name with the leading 'c' dropped (cInt -> "int",
 * cNull -> "null"), matching ALang.type. */
static av_type *lang_type(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)env;
    av_type *v = av_list_get(args, 0);
    if (v->type == C_TYPE_OBJECT) {
        ast_class *cd = (ast_class *)av_object_get_class_def((av_object *)v);
        return &av_string_new(cd != NULL ? cd->base.name : "object")->base.base;
    }
    const char *cn = c_type_name(v->type);   /* e.g. "cInt" */
    char buf[32];
    size_t j = 0;
    for (size_t i = 1; cn[i] != '\0' && j < sizeof(buf) - 1; i++) {
        buf[j++] = (char)tolower((unsigned char)cn[i]);
    }
    buf[j] = '\0';
    return &av_string_new(buf)->base.base;
}

/* A lowercased name with its first character dropped, as Java's
 * name().substring(1).toLowerCase() (cInt -> "int"). */
static av_type *short_lower(const char *n)
{
    size_t len = strlen(n);
    char *out = AUSSOM_ALLOC_ATOMIC(len > 0 ? len : 1);
    size_t j = 0;
    for (size_t i = 1; i < len; i++) out[j++] = (char)tolower((unsigned char)n[i]);
    out[j] = '\0';
    return &av_string_new(out)->base.base;
}

/* A lowercased name, as Java's name().toLowerCase(). */
static av_type *lower(const char *n)
{
    size_t len = strlen(n);
    char *out = AUSSOM_ALLOC_ATOMIC(len + 1);
    for (size_t i = 0; i <= len; i++) out[i] = (char)tolower((unsigned char)n[i]);
    return &av_string_new(out)->base.base;
}

static av_type *str_or_empty(const char *s)
{
    return &av_string_new(s != NULL ? s : "")->base.base;
}

/* astAussomDoc.getAussomdoc. */
static av_type *doc_aussomdoc(ast_aussom_doc *d)
{
    av_map *ret = av_map_new();
    av_map_put(ret, "aussomDocText", str_or_empty(ast_aussom_doc_get_text(d)));
    av_list *lst = av_list_new();
    size_t n = ast_aussom_doc_entry_count(d);
    for (size_t i = 0; i < n; i++) {
        doc_text *dt = ast_aussom_doc_entry_at(d, i);
        av_map *dobj = av_map_new();
        av_map_put(dobj, "type", str_or_empty(doc_type_name(dt->type)));
        av_map_put(dobj, "text", str_or_empty(dt->text));
        if (dt->type == DOC_TYPE_ANNOTATION) {
            av_map_put(dobj, "tagName", str_or_empty(dt->tag_name));
            av_map_put(dobj, "tagValue", str_or_empty(dt->value));
            av_map_put(dobj, "tagDescription", str_or_empty(dt->description));
        }
        av_list_add(lst, &dobj->base.base);
    }
    av_map_put(ret, "docList", &lst->base.base);
    return &ret->base.base;
}

/* astFunctDef.getAussomdoc. */
static av_type *funct_aussomdoc(ast_funct_def *fd)
{
    av_map *ret = av_map_new();
    av_map_put(ret, "name", str_or_empty(fd->base.name));
    av_map_put(ret, "isClosure", &av_bool_new(ast_funct_def_get_closure(fd))->base.base);

    const ast_node_array *args = ast_funct_def_args_list_args(fd->arg_list);
    av_list *cargs = av_list_new();
    for (size_t i = 0; i < args->count; i++) {
        ast_node *arg = args->items[i];
        av_map *am = av_map_new();
        av_map_put(am, "type", lower(ast_node_type_name(arg->type)));
        if (arg->type != AST_TYPE_ETCETERA) {
            av_map_put(am, "name", str_or_empty(arg->name));
            av_map_put(am, "specifiedType", short_lower(c_type_name(arg->prim_type)));
            if (arg->type != AST_TYPE_VAR) {
                av_map_put(am, "valueType", short_lower(c_type_name(arg->prim_type)));
                if (arg->type == AST_TYPE_STRING) {
                    av_map_put(am, "value", str_or_empty(ast_string_get_value((ast_string *)arg)));
                } else if (arg->type == AST_TYPE_BOOL) {
                    av_map_put(am, "value", &av_bool_new(ast_bool_get_value((ast_bool *)arg))->base.base);
                } else if (arg->type == AST_TYPE_INT) {
                    av_map_put(am, "value", &av_int_new(ast_int_get_value((ast_int *)arg))->base.base);
                } else if (arg->type == AST_TYPE_DOUBLE) {
                    av_map_put(am, "value", &av_double_new(ast_double_get_value((ast_double *)arg))->base.base);
                } else if (arg->type == AST_TYPE_NULL) {
                    av_map_put(am, "value", &av_null_new()->base.base);
                }
            }
        }
        av_list_add(cargs, &am->base.base);
    }
    av_map_put(ret, "args", &cargs->base.base);

    if (fd->base.doc_node != NULL)
        av_map_put(ret, "aussomDoc", doc_aussomdoc(fd->base.doc_node));
    return &ret->base.base;
}

/* ALang.getClassAussomdoc and astClass.getAussomdoc. Without the
 * permission Java returns null. For a class that does not exist, Java's
 * cls.getAussomdoc() throws a NullPointerException, which reaches the
 * script as EXTERN_INVOCATION_TARGET_EXCEPTION. */
static av_type *lang_getClassAussomdoc(av_type *self, environment *env, av_list *args)
{
    (void)self;
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "aussomdoc.class.getJson", false)) {
        return &av_null_new()->base.base;
    }
    const char *name = av_list_get_string(args, 0);
    ast_class *cls = (name != NULL) ? engine_resolve_class(eng, name) : NULL;
    if (cls == NULL) {
        av_exception *e = av_exception_new_type(AV_EX_RUNTIME);
        av_exception_set(e, -1, "EXTERN_INVOCATION_TARGET_EXCEPTION",
            "External call, invocation target exception for method "
            "'getClassAussomdoc', the external method threw an uncaught "
            "exception: java.lang.NullPointerException: Cannot invoke "
            "\"com.aussom.ast.astClass.getAussomdoc()\" because \"cls\" is null",
            "", "");
        return &e->base.base;
    }

    av_map *m = av_map_new();
    av_map_put(m, "type", &av_string_new("class")->base.base);
    av_map_put(m, "className", str_or_empty(cls->base.name));
    av_map_put(m, "fileName", str_or_empty(cls->base.file_name));
    av_map_put(m, "lineNumber", &av_int_new(cls->base.line_num)->base.base);
    av_map_put(m, "colNumber", &av_int_new(cls->base.col_num)->base.base);
    av_map_put(m, "isStatic", &av_bool_new(ast_class_get_static(cls))->base.base);
    av_map_put(m, "isExtern", &av_bool_new(ast_class_get_extern(cls))->base.base);
    av_map_put(m, "externClassName", str_or_empty(ast_class_get_extern_class_name(cls)));

    av_list *ext_list = av_list_new();
    for (size_t i = 0; i < cls->extended_classes.count; i++) {
        av_list_add(ext_list, &av_string_new(cls->extended_classes.items[i])->base.base);
    }
    av_map_put(m, "extendedClasses", &ext_list->base.base);

    if (cls->base.doc_node != NULL)
        av_map_put(m, "aussomDoc", doc_aussomdoc(cls->base.doc_node));

    /* membList: the class's own members, in declaration order. */
    av_list *members = av_list_new();
    for (size_t i = 0; i < cls->members.count; i++) {
        ast_node *mbr = cls->members.items[i].value;
        av_map *mm = av_map_new();
        av_map_put(mm, "name", &av_string_new(cls->members.items[i].name)->base.base);
        if (mbr == NULL || mbr->doc_node == NULL)
            av_map_put(mm, "value", &av_null_new()->base.base);
        else
            av_map_put(mm, "value", doc_aussomdoc(mbr->doc_node));
        av_list_add(members, &mm->base.base);
    }
    av_map_put(m, "members", &members->base.base);

    /* Functions: one entry per declared overload, from functList. */
    av_list *methods = av_list_new();
    size_t nf;
    ast_funct_def **fl = ast_class_get_all_functions(cls, &nf);
    for (size_t i = 0; i < nf; i++) av_list_add(methods, funct_aussomdoc(fl[i]));
    av_map_put(m, "methods", &methods->base.base);

    return &m->base.base;
}

/* --- exception accessors ----------------------------------------- */

static av_type *exc_getLineNumber(av_type *self, environment *e, av_list *a)
{ (void)e; (void)a; return &av_int_new(av_exception_get_line((av_exception *)self))->base.base; }

static av_type *exc_getExceptionType(av_type *self, environment *e, av_list *a)
{ (void)e; (void)a; return &av_string_new(av_ex_type_name(av_exception_get_et((av_exception *)self)))->base.base; }

static av_type *exc_str(const char *s) { return &av_string_new(s != NULL ? s : "")->base.base; }

static av_type *exc_getId(av_type *self, environment *e, av_list *a)
{ (void)e; (void)a; return exc_str(av_exception_get_id((av_exception *)self)); }

static av_type *exc_getText(av_type *self, environment *e, av_list *a)
{ (void)e; (void)a; return exc_str(av_exception_get_text((av_exception *)self)); }

static av_type *exc_getDetails(av_type *self, environment *e, av_list *a)
{ (void)e; (void)a; return exc_str(av_exception_get_details((av_exception *)self)); }

static av_type *exc_getStackTrace(av_type *self, environment *e, av_list *a)
{ (void)e; (void)a; return exc_str(av_exception_get_stack_trace((av_exception *)self)); }

const aussom_extern_method stdlib_alang_methods[] = {
    { "type", lang_type },
    { "getClassAussomdoc", lang_getClassAussomdoc },
    { NULL, NULL }
};

const aussom_extern_method stdlib_aussom_exception_methods[] = {
    { "getLineNumber", exc_getLineNumber },
    { "getExceptionType", exc_getExceptionType },
    { "getId", exc_getId },
    { "getText", exc_getText },
    { "getDetails", exc_getDetails },
    { "getStackTrace", exc_getStackTrace },
    { NULL, NULL }
};
