/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "aussom/ast_eval.h"
#include "aussom/ast_obj.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"
#include "aussom/oper_overload.h"
#include "aussom/av_exception.h"
#include "aussom/environment.h"

ast_obj *ast_obj_new(void)
{
    ast_obj *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_OBJ;
    n->index = NULL;
    return n;
}

void ast_obj_set_index(ast_obj *n, ast_node *index)
{
    n->index = index;
}

ast_node *ast_obj_get_index(const ast_obj *n)
{
    return n->index;
}

void ast_obj_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_obj *o = (const ast_obj *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    if (o->index != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"index\":\n", out);
        ast_node_to_string(o->index, level + 1, out);
        fputs(",\n", out);
    }
    if (n->child != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"child\":\n", out);
        ast_node_to_string(n->child, level + 1, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* --- object / index path ----------------------------------------- */

/* An exception of type `et` at `line`, with printf-style text. The text
 * may hold a script-supplied name or key of any length. */
static av_type *obj_exc(environment *env, int line, av_ex_type et,
                        const char *id, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *text = AUSSOM_ALLOC((size_t)(len > 0 ? len : 0) + 1);
    va_start(ap, fmt);
    vsnprintf(text, (size_t)(len > 0 ? len : 0) + 1, fmt, ap);
    va_end(ap);
    av_exception *e = av_exception_new_type(et);
    av_exception_set(e, line, id, text, text, environment_stack_trace(env));
    return &e->base.base;
}

/* Start of an object path: env has no current object yet. Resolves
 * `this`, locals, static classes, and (when getref) assignment
 * targets, in astObj.evalObjStart's order. */
static av_type *eval_obj_start(ast_node *n, environment *env, bool getref)
{
    av_strmap *locals = environment_locals(env);
    const char *name = n->name;

    if (strcmp(name, "this") == 0) {
        av_object *ci = environment_class_instance(env);
        if (ci == NULL) {
            return eval_make_exception(env, n->line_num, "UNDEFINED_NAME",
                "'this' used outside of a class instance.");
        }
        if (n->child != NULL) {
            environment *tenv = environment_clone(env, &ci->base);
            return ast_node_eval(n->child, tenv, getref);
        }
        return &ci->base;
    }

    if (!getref && locals != NULL) {
        av_type *local = av_strmap_get(locals, name);
        if (local != NULL) {
            if (n->child != NULL) {
                environment *tenv = environment_clone(env, local);
                return ast_node_eval(n->child, tenv, getref);
            }
            return local;
        }
    }

    /* Checked for a read or a write, so a static class name can't be
     * assigned to or used as a bare value. */
    if (engine_contains_static_class(environment_engine(env), name)) {
        av_type *sc = engine_get_static_class(environment_engine(env), name);
        if (n->child == NULL) {
            return obj_exc(env, n->line_num, AV_EX_RUNTIME, "NO_OPERATION",
                "astObj.evalObjStart(): Static class object found but no "
                "child function or property provided.");
        }
        if (n->child->type != AST_TYPE_FUNCTCALL
            && n->child->type != AST_TYPE_OBJ) {
            return obj_exc(env, n->line_num, AV_EX_INTERNAL, "NOT_IMPLEMENTED",
                "astObj.evalObjStart(): Static class with child of '%s' found.",
                ast_node_type_name(n->child->type));
        }
        environment *tenv = environment_clone(env, sc);
        return ast_node_eval(n->child, tenv, getref);
    }

    if (getref && n->child == NULL) {
        /* Assignment to a (possibly new) local. */
        if (locals != NULL && av_strmap_get(locals, name) == NULL) {
            av_strmap_put(locals, name, &av_null_new()->base.base);
        }
        av_ref *ref = av_ref_new();
        av_ref_set_map(ref, name, locals);
        return &ref->base;
    }

    if (getref && n->child != NULL && locals != NULL) {
        av_type *local = av_strmap_get(locals, name);
        if (local != NULL) {
            environment *tenv = environment_clone(env, local);
            return ast_node_eval(n->child, tenv, getref);
        }
    }

    return obj_exc(env, n->line_num, AV_EX_RUNTIME, "UNDEFINED_NAME",
        "astObj.evalObjStart(): Undefined name '%s'.", name);
}

/* Member access rule (astObj.memberHasAccess): an external accessor
 * (the calling instance is not the receiver) is denied a member the
 * receiver's class declares private. PROTECTED and undeclared (e.g.
 * inherited) members are allowed. Internal access (ci == receiver) is
 * always allowed. */
static bool member_has_access(environment *env, av_object *o, const char *name)
{
    if ((av_type *)environment_class_instance(env) == &o->base) return true;
    ast_class *cd = class_def_for(env, &o->base);
    if (cd == NULL) return true;
    ast_node *decl = ast_class_get_member(cd, name);
    /* An undeclared member is NOT granted public access: only a member
     * the class (or an ancestor) actually declares public is reachable
     * from outside. */
    if (decl == NULL) return false;
    return decl->access != ACCESS_PRIVATE;
}

/* Continuation of an object path: env->cur_obj is the value we are
 * accessing into. Handles object members, map keys, and list
 * indices. */
static av_type *eval_obj_cont(ast_node *n, environment *env, bool getref)
{
    av_type *cur = environment_cur_obj(env);
    const char *name = n->name;
    ast_obj *ao = (ast_obj *)n;

    /* Object member access. The member name is the node name, or a
     * bracket index value (obj["field"]). Private members are denied to
     * external accessors. */
    if (cur->type == C_TYPE_OBJECT) {
        av_object *o = (av_object *)cur;

        /* Overloaded index: a class defining __opIndex__ /
         * __opIndexSet__ takes over dereference for its instances, so
         * obj[key] no longer means "member named str(key)". Checked
         * first, and only for the bracket form (empty node name plus an
         * index). Classes defining neither keep the member behavior
         * below unchanged. */
        if (name[0] == '\0' && ao->index != NULL) {
            ast_class *icls = class_def_for(env, &o->base);
            bool has_idx = icls != NULL
                && ast_class_has_any_function_by_name(icls, OPER_OP_INDEX);
            bool has_idx_set = icls != NULL
                && ast_class_has_any_function_by_name(icls, OPER_OP_INDEX_SET);
            if (has_idx || has_idx_set) {
                environment *kenv = environment_clone(env, NULL);
                av_type *key = ast_node_eval(ao->index, kenv, false);
                if (av_is_ex(key)) return key;

                /* Terminal write target: hand back an index ref so
                 * eval_assignment calls __opIndexSet__. */
                if (getref && n->child == NULL) {
                    if (has_idx_set) return &av_index_ref_new(o, key)->base;
                    return obj_exc(env, n->line_num, AV_EX_RUNTIME,
                        "ASSIGN_NOT_POSSIBLE",
                        "astObj.evalObj(): Object '%s' overloads %s but not "
                        "%s, so index assignment isn't possible.",
                        icls->base.name, OPER_OP_INDEX, OPER_OP_INDEX_SET);
                }
                if (!has_idx) {
                    return obj_exc(env, n->line_num, AV_EX_RUNTIME,
                        "INDEX_NOT_FOUND",
                        "astObj.evalObj(): Object '%s' overloads %s but not "
                        "%s, so index reads aren't possible.",
                        icls->base.name, OPER_OP_INDEX_SET, OPER_OP_INDEX);
                }
                av_type *res = oper_call_one(env, n, "[]", o,
                                             OPER_OP_INDEX, key);
                if (av_is_ex(res)) return res;
                if (n->child != NULL) {
                    environment *tenv = environment_clone(env, res);
                    return ast_node_eval(n->child, tenv, getref);
                }
                return res;
            }
        }

        const char *mname = name;
        if (ao->index != NULL) {
            environment *ienv = environment_clone(env, NULL);
            av_type *ind = ast_node_eval(ao->index, ienv, false);
            if (av_is_ex(ind)) return ind;
            /* obj[key] names a member, so the key must be a string. */
            if (name[0] == '\0' && ind->type != C_TYPE_STRING) {
                return obj_exc(env, n->line_num, AV_EX_RUNTIME,
                    "INDEX_NOT_FOUND",
                    "astObj.evalObj(): Provided index isn't a string value.");
            }
            mname = eval_str(ind);
            if (mname == NULL) return eval_stack_overflow(env, n->line_num);
        }
        av_type *member = av_object_get_member(o, mname);
        if (member != NULL) {
            if (!member_has_access(env, o, mname)) {
                return obj_exc(env, n->line_num, AV_EX_RUNTIME, "NO_ACCESS",
                    "astObj.evalObj(): No access to member '%s'.", mname);
            }
            if (n->child != NULL) {
                environment *tenv = environment_clone(env, member);
                return ast_node_eval(n->child, tenv, getref);
            }
            if (getref) {
                av_ref *ref = av_ref_new();
                av_ref_set_map(ref, mname, av_object_members(o));
                return &ref->base;
            }
            return member;
        }

        /* Bracket access on an object that overloads neither index
         * method resolves a member by key, so a miss is specifically a
         * missing member rather than the generic unmatched-access
         * error the fall-through below would give. */
        if (name[0] == '\0' && ao->index != NULL) {
            return obj_exc(env, n->line_num, AV_EX_RUNTIME, "NO_MEMBER_FOUND",
                "astObj.evalObj(): Object doesn't have member '%s'.", mname);
        }
    }

    /* Map. The key is the node name, or the bracket index evaluated
     * and stringified. */
    if (cur->type == C_TYPE_MAP) {
        av_map *mp = (av_map *)cur;
        char *key = AUSSOM_STRDUP(name);
        if (ao->index != NULL) {
            environment *ienv = environment_clone(env, NULL);
            av_type *ind = ast_node_eval(ao->index, ienv, false);
            if (av_is_ex(ind)) return ind;
            key = eval_str(ind);
            if (key == NULL) return eval_stack_overflow(env, n->line_num);
        }
        if (n->child != NULL) {
            if (!av_map_contains(mp, key)) {
                return obj_exc(env, n->line_num, AV_EX_RUNTIME, "MAP_MISSING_KEY",
                    "astObj.evalObj(): Map doesn't have key '%s'.", key);
            }
            environment *tenv = environment_clone(env, av_map_get(mp, key));
            return ast_node_eval(n->child, tenv, getref);
        }
        if (getref) {
            av_ref *ref = av_ref_new();
            av_ref_set_map(ref, key, av_map_strmap(mp));
            return &ref->base;
        }
        if (av_map_contains(mp, key)) return av_map_get(mp, key);
        return obj_exc(env, n->line_num, AV_EX_RUNTIME, "MAP_MISSING_KEY",
            "astObj.evalObj(): Map doesn't have key '%s'.", key);
    }

    /* List. Requires a bracket index that resolves to an int. */
    if (cur->type == C_TYPE_LIST) {
        if (ao->index == NULL) {
            return eval_make_exception(env, n->line_num, "INDEX_NOT_FOUND",
                "astObj.evalObj(): List found but no index found.");
        }
        environment *ienv = environment_clone(env, NULL);
        av_type *ind = ast_node_eval(ao->index, ienv, false);
        if (av_is_ex(ind)) return ind;
        if (ind->type != C_TYPE_INT) {
            return eval_make_exception(env, n->line_num, "INDEX_NOT_FOUND",
                "astObj.evalObj(): Provided index isn't an integer value.");
        }
        int64_t i = av_int_get((av_int *)ind);
        av_list *lst = (av_list *)cur;
        size_t sz = av_list_size(lst);
        if (i < 0 || (size_t)i >= sz) {
            return eval_make_exception(env, n->line_num, "INDEX_OUT_OF_BOUNDS",
                "astObj.evalObj(): Index out of bounds.");
        }
        if (n->child != NULL) {
            environment *tenv = environment_clone(env, av_list_get(lst, (size_t)i));
            return ast_node_eval(n->child, tenv, getref);
        }
        if (getref) {
            av_ref *ref = av_ref_new();
            av_ref_set_list(ref, i, av_list_vec(lst));
            return &ref->base;
        }
        return av_list_get(lst, (size_t)i);
    }

    return obj_exc(env, n->line_num, AV_EX_INTERNAL, "NO_MEMBER_FOUND",
        "astObj.evalObj(): Unmatched object type for '%s'.", name);
}

av_type *ast_obj_eval(ast_node *n, environment *env, bool getref)
{
    if (environment_cur_obj(env) == NULL) {
        return eval_obj_start(n, env, getref);
    }
    return eval_obj_cont(n, env, getref);
}
