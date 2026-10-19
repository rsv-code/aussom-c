/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <ctype.h>
#include <math.h>
#include <string.h>
#include "aussom/ast_eval.h"
#include "aussom/ast_expression.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"
#include "aussom/oper_overload.h"
#include "aussom/parser.h"
#include "aussom/stdlib_sources.h"

ast_expression *ast_expression_new(void)
{
    ast_expression *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_EXP;
    n->left = NULL;
    n->right = NULL;
    n->e_type = EXP_UNDEF;
    return n;
}

ast_expression *ast_expression_new_with_left(ast_node *left)
{
    ast_expression *n = ast_expression_new();
    n->left = left;
    return n;
}

void ast_expression_set_left(ast_expression *n, ast_node *left)
{
    n->left = left;
}

void ast_expression_set_right(ast_expression *n, ast_node *right)
{
    n->right = right;
}

void ast_expression_set_etype(ast_expression *n, exp_type t)
{
    n->e_type = t;
}

ast_node *ast_expression_get_left(const ast_expression *n)
{
    return n->left;
}

ast_node *ast_expression_get_right(const ast_expression *n)
{
    return n->right;
}

exp_type ast_expression_get_etype(const ast_expression *n)
{
    return n->e_type;
}

void ast_expression_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_expression *e = (const ast_expression *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"eType\": \"%s\"\n", exp_type_name(e->e_type));
    if (e->left != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"left\":\n", out);
        ast_node_to_string(e->left, level + 1, out);
        fputs(",\n", out);
    }
    if (e->right != NULL) {
        ast_node_write_tabs(level + 1, out);
        fputs("\"right\":\n", out);
        ast_node_to_string(e->right, level + 1, out);
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

/* --- numeric helpers (mirror astExpression private helpers) ------ */

static bool is_number(const av_type *v)
{
    return v->type == C_TYPE_INT || v->type == C_TYPE_DOUBLE || v->type == C_TYPE_BOOL;
}

static bool is_int(const av_type *v)
{
    return v->type == C_TYPE_INT || v->type == C_TYPE_BOOL;
}

static int64_t get_int(const av_type *v)
{
    if (v->type == C_TYPE_INT) return av_int_get((const av_int *)v);
    if (v->type == C_TYPE_BOOL) return av_bool_get((const av_bool *)v) ? 1 : 0;
    return 0;
}

static double get_double(const av_type *v)
{
    if (v->type == C_TYPE_DOUBLE) return av_double_get((const av_double *)v);
    if (v->type == C_TYPE_INT) return (double)av_int_get((const av_int *)v);
    if (v->type == C_TYPE_BOOL) return av_bool_get((const av_bool *)v) ? 1.0 : 0.0;
    return 0.0;
}

static bool bool_val(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_NULL:   return false;
    case C_TYPE_BOOL:   return av_bool_get((const av_bool *)v);
    case C_TYPE_INT:    return av_int_get((const av_int *)v) != 0;
    case C_TYPE_DOUBLE: return av_double_get((const av_double *)v) != 0.0;
    case C_TYPE_STRING: return av_string_get((const av_string *)v)[0] != '\0';
    case C_TYPE_OBJECT:
    case C_TYPE_LIST:
    case C_TYPE_MAP:    return true;
    default:            return false;
    }
}

/* Appends "Define __opX__ ..." to an error when an operand is a class
 * instance that could have overloaded the operator but did not. Returns
 * "" for non-object operands so primitive error text is unchanged. */
static const char *op_hint(exp_type t, const av_type *l, const av_type *r)
{
    static _Thread_local char buf[192];
    if (l->type != C_TYPE_OBJECT && (r == NULL || r->type != C_TYPE_OBJECT)) return "";
    const char *name = (t == EXP_NOT) ? OPER_OP_NOT
                     : (t == EXP_COUNT) ? OPER_OP_COUNT
                     : oper_binary_method(t);
    if (name == NULL) return "";
    snprintf(buf, sizeof(buf),
        " Define %s in the class to overload the '%s' operator.",
        name, oper_symbol(t));
    return buf;
}

static av_type *make_ex_hint(environment *env, int line, const char *id,
                             const char *text, exp_type t,
                             const av_type *l, const av_type *r)
{
    char msg[320];
    snprintf(msg, sizeof(msg), "%s%s", text, op_hint(t, l, r));
    return eval_make_exception(env, line, id, msg);
}

/* --- bitwise & and | (new in 1.3.x) ------------------------------
 * Eager (both sides always evaluated), which is what makes them safe
 * to overload where && and || are not. Built-in behavior is bitwise
 * ONLY -- the language already has && / || for logic -- with bools
 * coerced to 0/1 like the arithmetic operators do. */

static av_type *bit_error(environment *env, int line, const char *fn,
                          const char *what, exp_type t, av_type *l, av_type *r)
{
    char msg[224];
    snprintf(msg, sizeof(msg),
        "astExpression.%s(): Bitwise %s requires int operands, found '%s' and '%s'.",
        fn, what, c_type_name(l->type), c_type_name(r->type));
    return make_ex_hint(env, line, "INVALID_EXPRESSION", msg, t, l, r);
}

/* astExpression's "<Left|Right> side of expression isn't a number"
 * error, naming the Java method (evalMinus, evalDiv, ...). */
static av_type *not_number(environment *env, int line, const char *fn,
                           exp_type t, av_type *l, av_type *r)
{
    bool left = !is_number(l);
    char msg[224];
    snprintf(msg, sizeof(msg),
        "astExpression.%s(): %s side of expression isn't a number, found type '%s'.",
        fn, left ? "Left" : "Right", c_type_name(left ? l->type : r->type));
    return make_ex_hint(env, line, "INVALID_EXPRESSION", msg, t, l, r);
}

static av_type *div_by_0(environment *env, int line, const char *fn)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "astExpression.%s(): Division by 0 exception.", fn);
    return eval_make_exception(env, line, "DIV_BY_0", msg);
}

static av_type *eval_bit_and(environment *env, int line, av_type *l, av_type *r)
{
    if (is_int(l) && is_int(r)) return &av_int_new(get_int(l) & get_int(r))->base.base;
    return bit_error(env, line, "evalBitAnd", "and (&)", EXP_ANDB, l, r);
}

static av_type *eval_bit_or(environment *env, int line, av_type *l, av_type *r)
{
    if (is_int(l) && is_int(r)) return &av_int_new(get_int(l) | get_int(r))->base.base;
    return bit_error(env, line, "evalBitOr", "or (|)", EXP_ORB, l, r);
}

/* --- binary numeric / concat operations -------------------------- */

/* Integer +, - and * wrap on overflow, as Java's long arithmetic does.
 * Signed overflow is undefined in C (an optimizer may assume it never
 * happens), so the arithmetic is done in uint64_t, where wrapping is
 * defined, and converted back. */
static int64_t wrap_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
static int64_t wrap_sub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
static int64_t wrap_mul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }

static av_type *eval_plus(environment *env, int line, av_type *l, av_type *r)
{
    (void)env; (void)line;
    if (is_number(l) && is_number(r)) {
        if (is_int(l) && is_int(r)) return &av_int_new(wrap_add(get_int(l), get_int(r)))->base.base;
        return &av_double_new(get_double(l) + get_double(r))->base.base;
    }
    /* Concatenation. */
    char *ls = eval_str(l);
    char *rs = (ls != NULL) ? eval_str(r) : NULL;
    if (rs == NULL) return eval_stack_overflow(env, line);
    size_t n = strlen(ls) + strlen(rs) + 1;
    char *buf = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(buf, n, "%s%s", ls, rs);
    return &av_string_new(buf)->base.base;
}

static av_type *eval_minus(environment *env, int line, av_type *l, av_type *r)
{
    if (!is_number(l) || !is_number(r))
        return not_number(env, line, "evalMinus", EXP_SUBTRACT, l, r);
    if (is_int(l) && is_int(r)) return &av_int_new(wrap_sub(get_int(l), get_int(r)))->base.base;
    return &av_double_new(get_double(l) - get_double(r))->base.base;
}

static av_type *eval_mult(environment *env, int line, av_type *l, av_type *r)
{
    if (!is_number(l) || !is_number(r))
        return not_number(env, line, "evalMult", EXP_MULTIPLY, l, r);
    if (is_int(l) && is_int(r)) return &av_int_new(wrap_mul(get_int(l), get_int(r)))->base.base;
    return &av_double_new(get_double(l) * get_double(r))->base.base;
}

static bool is_zero(const av_type *v)
{
    if (v->type == C_TYPE_INT) return get_int(v) == 0;
    if (v->type == C_TYPE_DOUBLE) return av_double_get((const av_double *)v) == 0.0;
    return false;
}

static av_type *eval_div(environment *env, int line, av_type *l, av_type *r)
{
    if (!is_number(l) || !is_number(r))
        return not_number(env, line, "evalDiv", EXP_DIVIDE, l, r);
    if (is_zero(r)) return div_by_0(env, line, "evalDiv");
    return &av_double_new(get_double(l) / get_double(r))->base.base;
}

static av_type *eval_floordiv(environment *env, int line, av_type *l, av_type *r)
{
    if (!is_number(l) || !is_number(r))
        return not_number(env, line, "evalFloorDiv", EXP_FLOORDIV, l, r);
    if (is_zero(r)) return div_by_0(env, line, "evalFloorDiv");
    if (is_int(l) && is_int(r)) {
        int64_t a = get_int(l), b = get_int(r);
        /* INT64_MIN / -1 is undefined in C and traps on x86-64. Java's
         * Math.floorDiv wraps it to INT64_MIN, so negate in unsigned. */
        if (b == -1) return &av_int_new((int64_t)(0 - (uint64_t)a))->base.base;
        int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) q--;  /* floor toward -inf */
        return &av_int_new(q)->base.base;
    }
    return &av_double_new(floor(get_double(l) / get_double(r)))->base.base;
}

static av_type *eval_modulus(environment *env, int line, av_type *l, av_type *r)
{
    if (!is_number(l) || !is_number(r))
        return not_number(env, line, "evalModulus", EXP_MODULUS, l, r);
    if (is_zero(r)) return div_by_0(env, line, "evalModulus");
    if (is_int(l) && is_int(r)) {
        /* INT64_MIN % -1 is undefined in C and traps on x86-64. Any
         * value modulo -1 is 0, which is what Java returns. */
        if (get_int(r) == -1) return &av_int_new(0)->base.base;
        return &av_int_new(get_int(l) % get_int(r))->base.base;
    }
    return &av_double_new(fmod(get_double(l), get_double(r)))->base.base;
}

static av_type *eval_eqeq(environment *env, int line, av_type *l, av_type *r)
{
    (void)env; (void)line;
    if (is_number(l) && is_number(r)) return &av_bool_new(get_double(l) == get_double(r))->base.base;
    if (l->type == C_TYPE_STRING || r->type == C_TYPE_STRING) {
        char *ls = eval_str(l);
        char *rs = (ls != NULL) ? eval_str(r) : NULL;
        if (rs == NULL) return eval_stack_overflow(env, line);
        return &av_bool_new(strcmp(ls, rs) == 0)->base.base;
    }
    if (l->type == C_TYPE_NULL && r->type == C_TYPE_NULL) return &av_bool_new(true)->base.base;
    return &av_bool_new(l == r)->base.base;
}

static av_type *eval_cmp(environment *env, int line, av_type *l, av_type *r, int op)
{
    /* op: 0 LT, 1 GT, 2 LTEQ, 3 GTEQ */
    if (is_number(l) && is_number(r)) {
        bool res;
        if (is_int(l) && is_int(r)) {
            int64_t a = get_int(l), b = get_int(r);
            res = (op == 0) ? a < b : (op == 1) ? a > b : (op == 2) ? a <= b : a >= b;
        } else {
            double a = get_double(l), b = get_double(r);
            res = (op == 0) ? a < b : (op == 1) ? a > b : (op == 2) ? a <= b : a >= b;
        }
        return &av_bool_new(res)->base.base;
    }
    if (l->type == C_TYPE_STRING && r->type == C_TYPE_STRING) {
        int64_t c = av_string_compare((av_string *)l, (av_string *)r);
        bool res = (op == 0) ? c < 0 : (op == 1) ? c > 0 : (op == 2) ? c <= 0 : c >= 0;
        return &av_bool_new(res)->base.base;
    }
    {
        static const char *const fn[] = { "evalLessThan", "evalGreaterThan",
                                          "evalLessThanEquals", "evalGreaterThanEquals" };
        static const char *const what[] = { "Less than", "Greater than",
                                             "Less than equals", "Greater than equals" };
        exp_type t = (op == 0) ? EXP_LT : (op == 1) ? EXP_GT
                   : (op == 2) ? EXP_LTEQ : EXP_GTEQ;
        char msg[256];
        snprintf(msg, sizeof(msg),
            "astExpression.%s(): %s comparison of non-number, found types '%s' and '%s'.",
            fn[op], what[op], c_type_name(l->type), c_type_name(r->type));
        return make_ex_hint(env, line, "INVALID_COMPARISON", msg, t, l, r);
    }
}

static av_type *eval_insert(environment *env, int line, av_type *l, av_type *r)
{
    if (l->type == C_TYPE_LIST) {
        av_list_add((av_list *)l, r);
        return l;
    }
    return make_ex_hint(env, line, "INSERT_NOT_POSSIBLE",
        "astExpression.evalInsert(): Left side of insert expression not a list.", EXP_INSERT, l, r);
}

/* --- assignment -------------------------------------------------- */

static av_type *eval_assignment(ast_expression *e, environment *env, bool getref)
{
    av_type *lres = ast_node_eval(e->left, env, true);
    if (av_is_ex(lres)) return lres;
    if (lres->type != C_TYPE_REF) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "astExpression.assignment(): Left side of assignment expression "
            "returned an object of type '%s' which can't be assigned.",
            c_type_name(lres->type));
        return eval_make_exception(env, e->base.line_num, "ASSIGN_NOT_POSSIBLE", msg);
    }

    /* Overloaded index write target: obj[key] = value where the class
     * defines __opIndexSet__. Call it here rather than through
     * av_ref_assign so exceptions from the method propagate. */
    av_ref *ref = (av_ref *)lres;
    if (ref->is_index) {
        av_type *ival = ast_node_eval(e->right, env, getref);
        if (av_is_ex(ival)) return ival;
        av_list *iargs = av_list_new();
        av_list_add(iargs, ref->ikey);
        av_list_add(iargs, ival);
        av_type *ires = oper_call(env, &e->base, "[]=", ref->itarget,
                                  OPER_OP_INDEX_SET, iargs);
        if (av_is_ex(ires)) return ires;
        return ival;
    }

    av_type *rval = ast_node_eval(e->right, env, getref);
    if (av_is_ex(rval)) return rval;
    av_ref_assign(ref, rval);
    return rval;
}

/* --- object init (:=) -------------------------------------------- */

/* astExpression.setSet's error for a key it can neither assign nor
 * pass to a setter. */
static av_type *init_val_not_found(environment *env, int line,
                                   const char *key, av_type *lres)
{
    size_t len = strlen(key) + strlen(eval_type_name(lres)) + 96;
    char *msg = AUSSOM_ALLOC_ATOMIC(len);
    snprintf(msg, len, "astExpression.init(): The provided init value '%s' "
             "not found in object '%s'.", key, eval_type_name(lres));
    return eval_make_exception(env, line, "INIT_VAL_NOT_FOUND", msg);
}

/* `obj := { "field": value, ... }` -- assigns each map entry to a
 * public member of the object, or routes through a `setField`
 * setter. Mirrors astExpression.set / setSet. */
static av_type *eval_set(ast_expression *e, environment *env, bool getref)
{
    int line = e->base.line_num;
    av_type *lres = ast_node_eval(e->left, env, false);
    if (av_is_ex(lres)) return lres;

    /* Every Aussom value is an AussomObject in Java, so only a control
     * carrier can fail Java's left-side check. */
    if (lres->type == C_TYPE_REF || lres->type == C_TYPE_BREAK
        || lres->type == C_TYPE_RETURN) {
        char msg[128];
        snprintf(msg, sizeof(msg), "astExpression.init(): Left side of init "
                 "expression returned invalid type '%s', expecting 'object'.",
                 c_type_name(lres->type));
        return eval_make_exception(env, line, "INIT_NOT_POSSIBLE", msg);
    }

    /* Overloaded init: a class defining __opSet__ takes over := and may
     * accept any right-side type (the built-in path requires a map).
     * The expression result stays the left object either way, so init
     * chains like `a = new vector() := {...}` keep working regardless
     * of what the method returns. */
    if (oper_has_op(env, lres, OPER_OP_SET)) {
        av_type *orres = ast_node_eval(e->right, env, false);
        if (av_is_ex(orres)) return orres;
        av_type *oret = oper_call_one(env, &e->base, ":=", (av_object *)lres,
                                      OPER_OP_SET, orres);
        if (av_is_ex(oret)) return oret;
        return lres;
    }

    av_type *rres = ast_node_eval(e->right, env, false);
    if (av_is_ex(rres)) return rres;
    if (rres->type != C_TYPE_MAP) {
        /* Java names the left side's type here, not the right's. */
        char msg[128];
        snprintf(msg, sizeof(msg), "astExpression.init(): Right side of init "
                 "expression returned invalid type '%s', expecting 'map'.",
                 c_type_name(lres->type));
        return eval_make_exception(env, line, "INIT_NOT_POSSIBLE", msg);
    }

    /* A value type (map, string, int, ...) has no members and, in Java,
     * no class definition: a short key fails the length check below and
     * any other key throws a NullPointerException from the setter
     * lookup. C reports INIT_VAL_NOT_FOUND for both. */
    bool is_obj = lres->type == C_TYPE_OBJECT;
    av_object *ao = is_obj ? (av_object *)lres : NULL;
    ast_class *cls = is_obj ? class_def_for(env, lres) : NULL;
    av_strmap *sm = av_map_strmap((av_map *)rres);

    /* Snapshot the map so we don't hold its lock across setter calls. */
    av_strmap_lock(sm);
    size_t n = av_strmap_size_locked(sm);
    const char **keys = AUSSOM_ALLOC(n * sizeof(*keys));
    av_type **vals = AUSSOM_ALLOC(n * sizeof(*vals));
    for (size_t i = 0; i < n; i++) {
        keys[i] = av_strmap_key_at_locked(sm, i);
        vals[i] = av_strmap_value_at_locked(sm, i);
    }
    av_strmap_unlock(sm);

    for (size_t i = 0; i < n; i++) {
        /* Only a member the class declares public is assigned directly,
         * even from inside the class; any other key needs a setter. */
        if (ao != NULL && cls != NULL && av_object_contains_member(ao, keys[i])) {
            ast_node *decl = ast_class_get_member(cls, keys[i]);
            if (decl != NULL && decl->access == ACCESS_PUBLIC) {
                av_object_add_member(ao, keys[i], vals[i]);
                continue;
            }
        }
        size_t klen = strlen(keys[i]);
        if (klen < 2 || cls == NULL) {
            return init_val_not_found(env, line, keys[i], lres);
        }
        /* setKey(value) */
        char *setter = AUSSOM_ALLOC_ATOMIC(3 + klen + 1);
        setter[0] = 's'; setter[1] = 'e'; setter[2] = 't';
        setter[3] = (char)toupper((unsigned char)keys[i][0]);
        memcpy(setter + 4, keys[i] + 1, klen);   /* copies rest + NUL */
        if (!ast_class_has_any_function_by_name(cls, setter)) {
            return init_val_not_found(env, line, keys[i], lres);
        }
        av_list *sargs = av_list_new();
        av_list_add(sargs, vals[i]);
        environment *tenv = environment_clone(env, lres);
        environment_set_class_instance(tenv, ao);
        av_type *sret = class_call(tenv, getref, cls, setter, sargs);
        if (av_is_ex(sret)) return sret;
    }
    return lres;
}

/* --- binary dispatch --------------------------------------------- */

/* Overload handling for == and !=. Equality calls the same method name
 * on either side (the argument is always the other operand). != prefers
 * __opNotEq__, then falls back to negating __opEq__, which then must
 * return a bool. Returns NULL when neither operand overloads, so the
 * built-in behavior runs. */
static av_type *call_equality_overload(ast_expression *e, environment *env,
                                       av_type *l, av_type *r)
{
    const char *glyph = oper_symbol(e->e_type);
    ast_node *site = &e->base;

    if (e->e_type != EXP_NOTEQ) {
        if (oper_has_op(env, l, OPER_OP_EQ))
            return oper_call_one(env, site, glyph, (av_object *)l, OPER_OP_EQ, r);
        if (oper_has_op(env, r, OPER_OP_EQ))
            return oper_call_one(env, site, glyph, (av_object *)r, OPER_OP_EQ, l);
        return NULL;
    }

    if (oper_has_op(env, l, OPER_OP_NOT_EQ))
        return oper_call_one(env, site, glyph, (av_object *)l, OPER_OP_NOT_EQ, r);
    if (oper_has_op(env, r, OPER_OP_NOT_EQ))
        return oper_call_one(env, site, glyph, (av_object *)r, OPER_OP_NOT_EQ, l);

    av_type *eq = NULL;
    if (oper_has_op(env, l, OPER_OP_EQ))
        eq = oper_call_one(env, site, glyph, (av_object *)l, OPER_OP_EQ, r);
    else if (oper_has_op(env, r, OPER_OP_EQ))
        eq = oper_call_one(env, site, glyph, (av_object *)r, OPER_OP_EQ, l);

    if (eq == NULL) return NULL;
    if (av_is_ex(eq)) return eq;
    if (eq->type == C_TYPE_BOOL)
        return &av_bool_new(!av_bool_get((av_bool *)eq))->base.base;

    char msg[256];
    snprintf(msg, sizeof(msg),
        "astExpression.callEqualityOverload(): %s returned type '%s'; '!=' needs a bool to negate. "
        "Define %s for non-bool equality results.",
        OPER_OP_EQ, c_type_name(eq->type), OPER_OP_NOT_EQ);
    return eval_make_exception(env, e->base.line_num,
        "OPERATOR_RETURN_NOT_BOOL", msg);
}

static av_type *eval_oper(ast_expression *e, environment *env, bool getref)
{
    av_type *l = ast_node_eval(e->left, env, getref);
    if (av_is_ex(l)) return l;

    /* Short-circuit AND / OR on the left operand. These are deliberately
     * NOT overloadable: their evaluation order must stay predictable.
     * Classes that want overloadable logic use the eager & and |. */
    if (e->e_type == EXP_AND && !bool_val(l)) return &av_bool_new(false)->base.base;
    if (e->e_type == EXP_OR && bool_val(l)) return &av_bool_new(true)->base.base;

    av_type *r = ast_node_eval(e->right, env, getref);
    if (av_is_ex(r)) return r;

    /* Operator overloading: a class-instance operand may define an
     * operator method. The left side wins, then the reflected (or
     * mirrored, for comparisons) form on the right side. When neither
     * overloads, the built-in behavior below runs unchanged -- which is
     * why existing scripts are unaffected. */
    if (e->e_type == EXP_EQEQ || e->e_type == EXP_NOTEQ) {
        av_type *oret = call_equality_overload(e, env, l, r);
        if (oret != NULL) return oret;
    } else {
        const char *op_name = oper_binary_method(e->e_type);
        if (op_name != NULL) {
            if (oper_has_op(env, l, op_name)) {
                return oper_call_one(env, &e->base, oper_symbol(e->e_type),
                                     (av_object *)l, op_name, r);
            }
            const char *refl = oper_reflected_method(e->e_type);
            if (oper_has_op(env, r, refl)) {
                return oper_call_one(env, &e->base, oper_symbol(e->e_type),
                                     (av_object *)r, refl, l);
            }
        }
    }

    int line = e->base.line_num;
    switch (e->e_type) {
    case EXP_ADD:        return eval_plus(env, line, l, r);
    case EXP_SUBTRACT:   return eval_minus(env, line, l, r);
    case EXP_MULTIPLY:   return eval_mult(env, line, l, r);
    case EXP_DIVIDE:     return eval_div(env, line, l, r);
    case EXP_FLOORDIV:   return eval_floordiv(env, line, l, r);
    case EXP_MODULUS:    return eval_modulus(env, line, l, r);
    case EXP_EQEQ:       return eval_eqeq(env, line, l, r);
    case EXP_NOTEQ: {
        av_type *eq = eval_eqeq(env, line, l, r);
        if (av_is_ex(eq)) return eq;
        return &av_bool_new(!av_bool_get((av_bool *)eq))->base.base;
    }
    case EXP_LT:         return eval_cmp(env, line, l, r, 0);
    case EXP_GT:         return eval_cmp(env, line, l, r, 1);
    case EXP_LTEQ:       return eval_cmp(env, line, l, r, 2);
    case EXP_GTEQ:       return eval_cmp(env, line, l, r, 3);
    case EXP_AND:        return &av_bool_new(bool_val(r))->base.base;
    case EXP_OR:         return &av_bool_new(bool_val(r))->base.base;
    case EXP_ANDB:       return eval_bit_and(env, line, l, r);
    case EXP_ORB:        return eval_bit_or(env, line, l, r);
    case EXP_INSERT:     return eval_insert(env, line, l, r);
    case EXP_INSTANCEOF: {
        /* Every value type embeds av_object except the control
         * carriers (ref/return/break); those can't be a subject. */
        if (l->type == C_TYPE_REF || l->type == C_TYPE_RETURN
            || l->type == C_TYPE_BREAK || l->type == C_TYPE_UNDEF) {
            return eval_make_exception(env, line, "UNEXPECTED_TYPE_FOUND",
                "astExpression.evalInstanceOf(): Left side of expression is not an instance of AussomObject.");
        }
        if (r->type != C_TYPE_STRING) {
            /* Java's text, though it is the right side that is wrong. */
            return eval_make_exception(env, line, "UNEXPECTED_TYPE_FOUND",
                "astExpression.evalInstanceOf(): Left side of expression is not an instance of AussomObject.");
        }
        const char *rname = av_string_get((av_string *)r);
        ast_class *cls = class_def_for(env, l);
        bool io = (cls != NULL) && ast_class_instance_of(cls, rname);
        if (!io) {
            /* Also accept the type short-name (cNull -> "null",
             * cInt -> "int"): how instanceof addresses the primitives. */
            const char *cn = c_type_name(l->type);
            char buf[32];
            size_t j = 0;
            for (size_t i = 1; cn[i] != '\0' && j < sizeof(buf) - 1; i++) {
                buf[j++] = (char)((cn[i] >= 'A' && cn[i] <= 'Z') ? cn[i] - 'A' + 'a' : cn[i]);
            }
            buf[j] = '\0';
            if (strcmp(buf, rname) == 0) io = true;
        }
        return &av_bool_new(io)->base.base;
    }
    default: {
        char msg[128];
        snprintf(msg, sizeof(msg), "Undefined operator expression '%s' found. (aExp::call)",
                 exp_type_name(e->e_type));
        av_exception *ex = av_exception_new_type(AV_EX_INTERNAL);
        av_exception_set(ex, line, "UNDEFINED_EXPRESSION", msg, msg,
                         environment_stack_trace(env));
        return &ex->base.base;
    }
    }
}

/* --- unary / left-only dispatch ---------------------------------- */

static av_type *eval_count(environment *env, int line, av_type *l)
{
    switch (l->type) {
    case C_TYPE_LIST:   return &av_int_new((int64_t)av_list_size((av_list *)l))->base.base;
    case C_TYPE_MAP:    return &av_int_new((int64_t)av_map_size((av_map *)l))->base.base;
    case C_TYPE_STRING: return &av_int_new((int64_t)av_string_length((av_string *)l))->base.base;
    default: {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "astExpression.evalCount(): Count operator (#) cannot be used for data type '%s'.",
            c_type_name(l->type));
        return make_ex_hint(env, line, "ASSIGN_NOT_POSSIBLE", msg,
                            EXP_COUNT, l, NULL);
    }
    }
}

static bool is_missnull_id(const char *id)
{
    return strcmp(id, "NO_MEMBER_FOUND") == 0
        || strcmp(id, "MAP_MISSING_KEY") == 0
        || strcmp(id, "INDEX_NOT_FOUND") == 0
        || strcmp(id, "INDEX_OUT_OF_BOUNDS") == 0;
}

static av_type *eval_missnull(ast_expression *e, environment *env, bool getref)
{
    av_type *l = ast_node_eval(e->left, env, getref);
    if (l->type == C_TYPE_EXCEPTION
        && is_missnull_id(av_exception_get_id((av_exception *)l))) {
        return &av_null_new()->base.base;
    }
    return l;
}

static av_type *eval_oper_left(ast_expression *e, environment *env, bool getref)
{
    int line = e->base.line_num;
    if (e->e_type == EXP_MISSNULL) {
        return eval_missnull(e, env, getref);
    }
    av_type *l = ast_node_eval(e->left, env, getref);
    if (av_is_ex(l)) return l;

    /* Unary overloading: __opNot__ / __opCount__ on class instances. */
    if (e->e_type == EXP_NOT && oper_has_op(env, l, OPER_OP_NOT)) {
        return oper_call(env, &e->base, "!", (av_object *)l, OPER_OP_NOT,
                         av_list_new());
    }
    if (e->e_type == EXP_COUNT && oper_has_op(env, l, OPER_OP_COUNT)) {
        return oper_call(env, &e->base, "#", (av_object *)l, OPER_OP_COUNT,
                         av_list_new());
    }

    switch (e->e_type) {
    case EXP_NOT:   return &av_bool_new(!bool_val(l))->base.base;
    case EXP_COUNT: return eval_count(env, line, l);
    case EXP_INCLUDE: {
        if (l->type != C_TYPE_STRING) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "astExpression.evalInclude(): Expecting string data type but found '%s' instead.",
                c_type_name(l->type));
            return eval_make_exception(env, line, "UNEXPECTED_DATA_TYPE", msg);
        }
        const char *path = av_string_get((av_string *)l);
        /* Ports astExpression.evalInclude: Engine.addInclude, with a
         * refusal returned as an exception. */
        engine *ieng = environment_engine(env);
        const char *err = engine_add_include(ieng, path);
        /* An error the included source's parse stopped with is what
         * Java's addInclude throws, so it fails the same way. */
        if (err == NULL) err = engine_parse_exception(ieng);
        if (err != NULL) {
            char msg[1024];
            snprintf(msg, sizeof(msg),
                "astExpression.evalInclude(): Failed to include '%s'. %s",
                path, err);
            return eval_make_exception(env, line, "INCLUDE_FAILED", msg);
        }
        return &av_bool_new(true)->base.base;
    }
    default: {
        char msg[128];
        snprintf(msg, sizeof(msg), "Undefined oper left expression '%s' found. (aExp::call)",
                 exp_type_name(e->e_type));
        av_exception *ex = av_exception_new_type(AV_EX_INTERNAL);
        av_exception_set(ex, line, "UNDEFINED_EXPRESSION", msg, msg,
                         environment_stack_trace(env));
        return &ex->base.base;
    }
    }
}

/* --- entry ------------------------------------------------------- */

av_type *ast_expression_eval(ast_expression *e, environment *env, bool getref)
{
    av_type *ret;

    if (e->left != NULL && e->right != NULL) {
        switch (e->e_type) {
        case EXP_ASSIGNMENT:
            ret = eval_assignment(e, env, getref);
            break;
        case EXP_SET:
            ret = eval_set(e, env, getref);
            break;
        default:
            ret = eval_oper(e, env, getref);
            break;
        }
    } else if (e->left != NULL) {
        ret = eval_oper_left(e, env, getref);
    } else {
        ret = &av_null_new()->base.base;
    }

    /* Child chaining: descend a trailing `.x` / `[i]` / method call on
     * the result. Every value type is object-derived and can receive
     * a method, so set it as the current object (e.g.
     * `(0-5).signum()`). Only control carriers never chain. */
    if (!av_is_ex(ret) && e->base.child != NULL
        && ret->type != C_TYPE_REF && ret->type != C_TYPE_RETURN
        && ret->type != C_TYPE_BREAK) {
        environment *tenv = environment_clone(env, ret);
        ret = ast_node_eval(e->base.child, tenv, getref);
    }
    return ret;
}
