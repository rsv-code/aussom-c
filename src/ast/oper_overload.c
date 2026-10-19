/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Ports com.aussom.ast.OperOverload.
 */

#include <string.h>

#include "aussom/ast_eval.h"
#include "aussom/oper_overload.h"

#include "aussom/aussom_gc.h"

const char *oper_binary_method(exp_type t)
{
    switch (t) {
    case EXP_ADD:      return "__opAdd__";
    case EXP_SUBTRACT: return "__opSub__";
    case EXP_MULTIPLY: return "__opMul__";
    case EXP_DIVIDE:   return "__opDiv__";
    case EXP_FLOORDIV: return "__opFloorDiv__";
    case EXP_MODULUS:  return "__opMod__";
    case EXP_EQEQ:     return OPER_OP_EQ;
    case EXP_NOTEQ:    return OPER_OP_NOT_EQ;
    case EXP_LT:       return "__opLt__";
    case EXP_GT:       return "__opGt__";
    case EXP_LTEQ:     return "__opLtEq__";
    case EXP_GTEQ:     return "__opGtEq__";
    case EXP_INSERT:   return "__opInsert__";
    case EXP_ANDB:     return "__opAnd__";
    case EXP_ORB:      return "__opOr__";
    default:           return NULL;
    }
}

const char *oper_reflected_method(exp_type t)
{
    switch (t) {
    case EXP_ADD:      return "__opRightAdd__";
    case EXP_SUBTRACT: return "__opRightSub__";
    case EXP_MULTIPLY: return "__opRightMul__";
    case EXP_DIVIDE:   return "__opRightDiv__";
    case EXP_FLOORDIV: return "__opRightFloorDiv__";
    case EXP_MODULUS:  return "__opRightMod__";
    case EXP_ANDB:     return "__opRightAnd__";
    case EXP_ORB:      return "__opRightOr__";
    /* Mirrored comparisons: no dedicated reflected names. */
    case EXP_LT:       return "__opGt__";
    case EXP_GT:       return "__opLt__";
    case EXP_LTEQ:     return "__opGtEq__";
    case EXP_GTEQ:     return "__opLtEq__";
    default:           return NULL;
    }
}

const char *oper_symbol(exp_type t)
{
    switch (t) {
    case EXP_ADD:      return "+";
    case EXP_SUBTRACT: return "-";
    case EXP_MULTIPLY: return "*";
    case EXP_DIVIDE:   return "/";
    case EXP_FLOORDIV: return "~/";
    case EXP_MODULUS:  return "%";
    case EXP_EQEQ:     return "==";
    case EXP_NOTEQ:    return "!=";
    case EXP_LT:       return "<";
    case EXP_GT:       return ">";
    case EXP_LTEQ:     return "<=";
    case EXP_GTEQ:     return ">=";
    case EXP_INSERT:   return "@=";
    case EXP_ANDB:     return "&";
    case EXP_ORB:      return "|";
    case EXP_SET:      return ":=";
    case EXP_NOT:      return "!";
    case EXP_COUNT:    return "#";
    default:           return "?";
    }
}

bool oper_has_op(environment *env, const av_type *val, const char *name)
{
    if (name == NULL) return false;
    if (val->type != C_TYPE_OBJECT) return false;
    ast_class *def = (ast_class *)av_object_get_class_def((av_object *)val);
    if (def == NULL) return false;
    return ast_class_has_any_function_by_name(def, name);
}

av_type *oper_call(environment *env, ast_node *site, const char *glyph,
                   av_object *target, const char *name, av_list *args)
{
    ast_class *def = (ast_class *)av_object_get_class_def(target);

    /* Push a call-site frame, matching what a normal method call does,
     * so the trace shows the line the operator was written on. */
    char note[96];
    snprintf(note, sizeof(note), "Operator '%s' called.", glyph);
    callstack *cst = callstack_new_frame(site->file_name, site->line_num,
                                         def->base.name, name,
                                         AUSSOM_STRDUP(note));
    callstack_set_parent(cst, environment_callstack(env));

    environment *tenv = environment_clone(env, &target->base);
    environment_set(tenv, target, environment_locals(env), cst);
    av_type *ret = class_call(tenv, false, def, name, args);

    /* A resolution failure on the operator method itself reads poorly
     * without operator context. Reword it in place; errors raised
     * deeper inside the method body pass through untouched. */
    if (av_is_ex(ret) && ret->type == C_TYPE_EXCEPTION) {
        av_exception *ex = (av_exception *)ret;
        const char *id = av_exception_get_id(ex);
        const char *txt = av_exception_get_text(ex);
        bool resolution_failure = id != NULL
            && (strcmp(id, "FUNCT_NOT_FOUND") == 0
                || strcmp(id, "AMBIGUOUS_OVERLOAD") == 0);
        if (resolution_failure && txt != NULL && strstr(txt, name) != NULL) {
            size_t n = strlen(glyph) + strlen(def->base.name) + strlen(txt) + 32;
            char *ctx = AUSSOM_ALLOC_ATOMIC(n);
            snprintf(ctx, n, "Operator '%s' on object '%s': %s",
                     glyph, def->base.name, txt);
            av_exception_set(ex, site->line_num, id, ctx, ctx,
                             av_exception_get_stack_trace(ex));
        }
    }
    return ret;
}

av_type *oper_call_one(environment *env, ast_node *site, const char *glyph,
                       av_object *target, const char *name, av_type *arg)
{
    av_list *args = av_list_new();
    av_list_add(args, arg);
    return oper_call(env, site, glyph, target, name, args);
}
