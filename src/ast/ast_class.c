/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "aussom/extern_registry.h"
#include "aussom/ast_eval.h"
#include "aussom/ast_class.h"
#include "aussom/security_manager.h"

#include "aussom/ast_dispatch.h"
#include "aussom/aussom_gc.h"
#include "aussom/stack_guard.h"
#include "aussom/mock.h"
#include "aussom/memstream.h"

static void member_array_add(ast_class_member_array *a, const char *name, ast_node *value)
{
    if (a->count == a->capacity) {
        size_t new_cap = (a->capacity == 0) ? 4 : a->capacity * 2;
        a->items = AUSSOM_REALLOC(a->items, new_cap * sizeof(*a->items));
        a->capacity = new_cap;
    }
    a->items[a->count].name = AUSSOM_STRDUP(name);
    a->items[a->count].value = value;
    a->count++;
}

static void string_array_add(ast_class_string_array *a, const char *s)
{
    if (a->count == a->capacity) {
        size_t new_cap = (a->capacity == 0) ? 4 : a->capacity * 2;
        a->items = AUSSOM_REALLOC(a->items, new_cap * sizeof(*a->items));
        a->capacity = new_cap;
    }
    a->items[a->count++] = AUSSOM_STRDUP(s);
}

static bool string_array_contains(const ast_class_string_array *a, const char *s)
{
    for (size_t i = 0; i < a->count; i++) {
        if (strcmp(a->items[i], s) == 0) return true;
    }
    return false;
}

/* --- function and member tables (ports astClass's fields) ---------
 * The maps are av_strmaps without a lock: a table is filled by one
 * thread (the parser, or class_init under its lock) and only read once
 * it is published. The dispatch map's values are ast_funct_def
 * pointers and the member map's are ast_node pointers, stored in the
 * map's av_type * slot and never used as values. The two sets store a
 * marker. */

typedef struct funct_vec {
    ast_funct_def **items;
    size_t count;
    size_t capacity;
} funct_vec;

struct ast_class_tables {
    funct_vec funct_list;            /* functList */
    av_strmap *dispatch;             /* dispatchMap: "name#sig" -> def */
    funct_vec wildcards;             /* wildcardOverloads */
    funct_vec variadics;             /* variadicOverloads */
    av_strmap *declared_names;       /* declaredNames */
    av_strmap *null_ambiguous;       /* nullAmbiguousKeys */
    av_strmap *memb_defs;            /* membDefs: name -> node */
    ast_class_string_array all_extended;  /* allExtendedClasses */
};

static av_type *const SET_MARK = (av_type *)&SET_MARK;

static void funct_vec_add(funct_vec *v, ast_funct_def *fd)
{
    if (v->count == v->capacity) {
        v->capacity = (v->capacity == 0) ? 8 : v->capacity * 2;
        v->items = AUSSOM_REALLOC(v->items, v->capacity * sizeof(*v->items));
    }
    v->items[v->count++] = fd;
}

static bool funct_vec_contains(const funct_vec *v, const ast_funct_def *fd)
{
    for (size_t i = 0; i < v->count; i++) {
        if (v->items[i] == fd) return true;
    }
    return false;
}

static void funct_vec_copy(funct_vec *dst, const funct_vec *src)
{
    dst->items = NULL;
    dst->count = 0;
    dst->capacity = 0;
    for (size_t i = 0; i < src->count; i++) funct_vec_add(dst, src->items[i]);
}

static av_strmap *strmap_copy(av_strmap *src)
{
    av_strmap *m = av_strmap_new_unlocked();
    for (size_t i = 0; i < src->order_count; i++) {
        av_strmap_put(m, av_strmap_key_at_locked(src, i),
                      av_strmap_value_at_locked(src, i));
    }
    return m;
}

static ast_class_tables *tables_new(void)
{
    ast_class_tables *t = AUSSOM_ALLOC(sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->dispatch = av_strmap_new_unlocked();
    t->declared_names = av_strmap_new_unlocked();
    t->null_ambiguous = av_strmap_new_unlocked();
    t->memb_defs = av_strmap_new_unlocked();
    return t;
}

static ast_class_tables *tables_copy(const ast_class_tables *src)
{
    ast_class_tables *t = AUSSOM_ALLOC(sizeof(*t));
    memset(t, 0, sizeof(*t));
    funct_vec_copy(&t->funct_list, &src->funct_list);
    funct_vec_copy(&t->wildcards, &src->wildcards);
    funct_vec_copy(&t->variadics, &src->variadics);
    t->dispatch = strmap_copy(src->dispatch);
    t->declared_names = strmap_copy(src->declared_names);
    t->null_ambiguous = strmap_copy(src->null_ambiguous);
    t->memb_defs = strmap_copy(src->memb_defs);
    for (size_t i = 0; i < src->all_extended.count; i++)
        string_array_add(&t->all_extended, src->all_extended.items[i]);
    return t;
}

ast_class_tables *ast_class_tables_of(const ast_class *n)
{
    return __atomic_load_n(&n->tables, __ATOMIC_ACQUIRE);
}

static ast_funct_def *dispatch_get(const ast_class_tables *t, const char *key)
{
    return (ast_funct_def *)av_strmap_get(t->dispatch, key);
}

ast_class *ast_class_new(void)
{
    ast_class *n = AUSSOM_ALLOC(sizeof(*n));
    ast_node_init(&n->base);
    n->base.type = AST_TYPE_CLASS;
    n->is_static = false;
    n->is_extern = false;
    n->extern_class_name = AUSSOM_STRDUP("");
    n->bound_extern = NULL;
    n->extended_classes.items = NULL;
    n->extended_classes.count = 0;
    n->extended_classes.capacity = 0;
    n->members.items = NULL;
    n->members.count = 0;
    n->members.capacity = 0;
    ast_node_array_init(&n->functions);
    n->tables = tables_new();
    n->init_ran = false;
    return n;
}

ast_class *ast_class_new_named(const char *name)
{
    ast_class *n = ast_class_new();
    ast_class_set_name(n, name);
    return n;
}

void ast_class_set_name(ast_class *n, const char *name)
{
    const char *safe = (name != NULL) ? name : "";
    /* Match Java side: any class other than "object" implicitly
     * extends "object". */
    if (strcmp(safe, "object") != 0
        && !string_array_contains(&n->extended_classes, "object")) {
        string_array_add(&n->extended_classes, "object");
    }
    ast_node_set_name(&n->base, safe);
}

void ast_class_set_extended_classes(ast_class *n, char *const *names, size_t count)
{
    n->extended_classes.items = NULL;
    n->extended_classes.count = 0;
    n->extended_classes.capacity = 0;
    for (size_t i = 0; i < count; i++) {
        string_array_add(&n->extended_classes, names[i]);
        if (!string_array_contains(&n->tables->all_extended, names[i]))
            string_array_add(&n->tables->all_extended, names[i]);
    }
}

void ast_class_add_member(ast_class *n, const char *member_name, ast_node *value)
{
    member_array_add(&n->members, member_name, value);
    av_strmap_put(n->tables->memb_defs, member_name, (av_type *)value);
}

/* astClass.isRefShape. */
static bool ref_shape(c_type t)
{
    return t == C_TYPE_STRING || t == C_TYPE_LIST || t == C_TYPE_MAP
        || t == C_TYPE_OBJECT || t == C_TYPE_CALLBACK;
}

/* astClass.computeSlotTags: the tags a declared slot is registered
 * under. A typed reference slot also accepts 'n' (null). A slot whose
 * default is null accepts 'n' in addition to its declared type, or
 * only 'n' with no declared type. Writes up to two tags. */
static size_t slot_tags(const ast_node *arg, char tags[2])
{
    c_type pt = arg->prim_type;
    if (arg->type == AST_TYPE_VAR) {
        if (pt != C_TYPE_UNDEF) {
            tags[0] = ast_funct_def_mangle_char(pt);
            if (ref_shape(pt)) { tags[1] = 'n'; return 2; }
            return 1;
        }
        tags[0] = '*';
        return 1;
    }
    if (arg->type == AST_TYPE_NULL) {
        if (pt != C_TYPE_UNDEF) {
            tags[0] = ast_funct_def_mangle_char(pt);
            tags[1] = 'n';
            return 2;
        }
        tags[0] = 'n';
        return 1;
    }
    /* Literal default: the literal's type. */
    c_type eff = ast_funct_def_effective_arg_type(arg);
    tags[0] = ast_funct_def_mangle_char(eff);
    if (ref_shape(eff)) { tags[1] = 'n'; return 2; }
    return 1;
}

/* astClass.headSignature: a variadic signature without its "...". */
static char *head_signature(const char *sig)
{
    size_t n = strlen(sig);
    char *out = AUSSOM_STRDUP(sig);
    if (n >= 4 && strcmp(sig + n - 4, ",...") == 0) out[n - 4] = '\0';
    else if (n >= 3 && strcmp(sig + n - 3, "...") == 0) out[n - 3] = '\0';
    return out;
}

/* astClass.registerSigVariants: registers `fd` under every "name#sig"
 * key for `arity` slots, the product of each slot's tags. Returns the
 * earlier overload when the primary variant (every slot at its
 * declared tag) is already taken, with *dup_sig set to that key's
 * signature. */
static ast_funct_def *register_sig_variants(ast_class_tables *t, const char *name,
                                            ast_funct_def *fd, char (*tags)[2],
                                            const size_t *ntags, int arity,
                                            char **dup_sig)
{
    size_t variants = 1;
    for (int i = 0; i < arity; i++) variants *= ntags[i];
    size_t nlen = strlen(name);
    char *key = AUSSOM_ALLOC_ATOMIC(nlen + 2 + (size_t)arity * 2 + 1);
    for (size_t v = 0; v < variants; v++) {
        memcpy(key, name, nlen);
        size_t k = nlen;
        key[k++] = '#';
        size_t rem = v;
        /* A variant is "primary" when every slot uses its declared tag
         * (index 0); a higher index means the slot fell back to 'n'. */
        bool primary = true;
        for (int i = 0; i < arity; i++) {
            size_t idx = rem % ntags[i];
            rem /= ntags[i];
            if (idx != 0) primary = false;
            if (i > 0) key[k++] = ',';
            key[k++] = tags[i][idx];
        }
        key[k] = '\0';
        if (av_strmap_contains(t->null_ambiguous, key)) {
            /* Already ambiguous for a null argument in this slot; a
             * third-or-later reference overload changes nothing. */
            continue;
        }
        ast_funct_def *prior = dispatch_get(t, key);
        if (prior != NULL && prior != fd) {
            /* A collision on the primary variant is a genuine duplicate.
             * One on a null-fallback variant is not: s(string) and
             * s(list) both accept null in that slot but are still told
             * apart by a non-null argument. Only a null argument is
             * ambiguous between them, so the key is dropped and marked,
             * and a null call reports the ambiguity unless a wildcard or
             * variadic overload takes it. */
            if (primary) {
                *dup_sig = AUSSOM_STRDUP(key + nlen + 1);
                return prior;
            }
            av_strmap_remove(t->dispatch, key);
            av_strmap_put(t->null_ambiguous, key, SET_MARK);
            continue;
        }
        av_strmap_put(t->dispatch, key, (av_type *)fd);
    }
    return NULL;
}

/* astClass.classifyAndAdd: files `fd` as a variadic, wildcard or exact
 * overload, refusing one that duplicates an earlier overload. */
static ast_funct_def *classify_and_add(ast_class_tables *t, const char *name,
                                       ast_funct_def *fd, char **dup_sig)
{
    char *sig = ast_funct_def_signature(fd);
    if (ast_funct_def_is_variadic(fd)) {
        char *head = head_signature(sig);
        for (size_t i = 0; i < t->variadics.count; i++) {
            ast_funct_def *ex = t->variadics.items[i];
            if (strcmp(ex->base.name, name) == 0
                && strcmp(head_signature(ast_funct_def_signature(ex)), head) == 0) {
                size_t hl = strlen(head);
                char *ds = AUSSOM_ALLOC_ATOMIC(hl + 5);
                if (hl == 0) memcpy(ds, "...", 4);
                else snprintf(ds, hl + 5, "%s,...", head);
                *dup_sig = ds;
                return ex;
            }
        }
        funct_vec_add(&t->variadics, fd);
    } else if (ast_funct_def_has_wildcard(fd)) {
        for (size_t i = 0; i < t->wildcards.count; i++) {
            ast_funct_def *ex = t->wildcards.items[i];
            if (strcmp(ex->base.name, name) == 0
                && strcmp(ast_funct_def_signature(ex), sig) == 0) {
                *dup_sig = sig;
                return ex;
            }
        }
        funct_vec_add(&t->wildcards, fd);
    } else {
        const ast_node_array *declared = ast_funct_def_args_list_args(fd->arg_list);
        int min = ast_funct_def_min_arity(fd);
        int max = ast_funct_def_max_arity(fd);
        char (*tags)[2] = AUSSOM_ALLOC_ATOMIC((size_t)(max > 0 ? max : 1) * sizeof(*tags));
        size_t *ntags = AUSSOM_ALLOC_ATOMIC((size_t)(max > 0 ? max : 1) * sizeof(*ntags));
        for (int i = 0; i < max; i++) ntags[i] = slot_tags(declared->items[i], tags[i]);
        for (int arity = min; arity <= max; arity++) {
            ast_funct_def *prior = register_sig_variants(t, name, fd, tags, ntags,
                                                         arity, dup_sig);
            if (prior != NULL) return prior;
        }
    }
    return NULL;
}

ast_funct_def *ast_class_add_function(ast_class *n, ast_funct_def *funct,
                                      char **dup_sig)
{
    ast_class_tables *t = n->tables;
    const char *name = funct->base.name;
    ast_funct_def *prior = classify_and_add(t, name, funct, dup_sig);
    if (prior != NULL) return prior;
    ast_node_array_add(&n->functions, &funct->base);
    funct_vec_add(&t->funct_list, funct);
    av_strmap_put(t->declared_names, name, SET_MARK);
    return NULL;
}

bool ast_class_contains_function(const ast_class *n, const char *name, const char *sig)
{
    return ast_class_get_funct(n, name, sig) != NULL;
}

ast_funct_def *ast_class_get_funct(const ast_class *n, const char *name, const char *sig)
{
    if (sig == NULL) sig = "";
    size_t len = strlen(name) + strlen(sig) + 2;
    char *key = AUSSOM_ALLOC_ATOMIC(len);
    snprintf(key, len, "%s#%s", name, sig);
    return dispatch_get(ast_class_tables_of(n), key);
}

ast_funct_def **ast_class_get_functions_by_name(const ast_class *n, const char *name,
                                               size_t *count)
{
    const ast_class_tables *t = ast_class_tables_of(n);
    funct_vec out = { NULL, 0, 0 };
    for (size_t i = 0; i < t->funct_list.count; i++) {
        if (strcmp(t->funct_list.items[i]->base.name, name) == 0)
            funct_vec_add(&out, t->funct_list.items[i]);
    }
    *count = out.count;
    return out.items;
}

bool ast_class_has_any_function_by_name(const ast_class *n, const char *name)
{
    return av_strmap_contains(ast_class_tables_of(n)->declared_names, name);
}

ast_funct_def **ast_class_get_all_functions(const ast_class *n, size_t *count)
{
    const ast_class_tables *t = ast_class_tables_of(n);
    *count = t->funct_list.count;
    return t->funct_list.items;
}

ast_funct_def **ast_class_tables_functions(const ast_class_tables *t, size_t *count)
{
    *count = t->funct_list.count;
    return t->funct_list.items;
}

bool ast_class_is_closure_function(const ast_class *n, const char *name)
{
    const ast_class_tables *t = ast_class_tables_of(n);
    if (!av_strmap_contains(t->declared_names, name)) return false;
    for (size_t i = 0; i < t->funct_list.count; i++) {
        ast_funct_def *fd = t->funct_list.items[i];
        if (fd->is_closure && strcmp(fd->base.name, name) == 0) return true;
    }
    return false;
}

bool ast_class_contains_member(const ast_class *n, const char *name)
{
    return av_strmap_contains(ast_class_tables_of(n)->memb_defs, name);
}

ast_node *ast_class_get_member(const ast_class *n, const char *name)
{
    return (ast_node *)av_strmap_get(ast_class_tables_of(n)->memb_defs, name);
}

size_t ast_class_get_members(const ast_class *n, const char ***names, ast_node ***decls)
{
    const ast_class_tables *t = ast_class_tables_of(n);
    size_t count = t->memb_defs->order_count;
    const char **nm = AUSSOM_ALLOC(count * sizeof(*nm) + 1);
    ast_node **dc = AUSSOM_ALLOC(count * sizeof(*dc) + 1);
    for (size_t i = 0; i < count; i++) {
        nm[i] = av_strmap_key_at_locked(t->memb_defs, i);
        dc[i] = (ast_node *)av_strmap_value_at_locked(t->memb_defs, i);
    }
    *names = nm;
    *decls = dc;
    return count;
}

bool ast_class_instance_of(const ast_class *n, const char *name)
{
    if (strcmp(n->base.name, name) == 0) return true;
    if (string_array_contains(&ast_class_tables_of(n)->all_extended, name)) return true;
    return strcmp(n->base.name, "cnull") == 0 && strcmp(name, "null") == 0;
}

void ast_class_set_static(ast_class *n, bool is_static)
{
    n->is_static = is_static;
}

bool ast_class_get_static(const ast_class *n)
{
    return n->is_static;
}

void ast_class_set_extern(ast_class *n, bool is_extern)
{
    n->is_extern = is_extern;
}

bool ast_class_get_extern(const ast_class *n)
{
    return n->is_extern;
}

void ast_class_set_extern_class_name(ast_class *n, const char *name)
{
    n->extern_class_name = AUSSOM_STRDUP(name);
}

const char *ast_class_get_extern_class_name(const ast_class *n)
{
    return n->extern_class_name;
}

void ast_class_set_bound_extern(ast_class *n, const extern_class *ec)
{
    n->bound_extern = ec;
}

const extern_class *ast_class_get_bound_extern(const ast_class *n)
{
    return n->bound_extern;
}

void ast_class_write_body(const ast_node *n, int level, FILE *out)
{
    const ast_class *c = (const ast_class *)n;
    ast_node_write_tabs(level, out);
    fputs("{\n", out);
    ast_node_write_node_str(n, level + 1, out);
    fputs(",\n", out);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"fileName\": \"%s\",\n", n->file_name);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"modRef\": \"%s\",\n", c->extern_class_name);
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"static\": \"%s\",\n", c->is_static ? "true" : "false");
    ast_node_write_tabs(level + 1, out);
    fprintf(out, "\"extern\": \"%s\",\n", c->is_extern ? "true" : "false");

    ast_node_write_tabs(level + 1, out);
    fputs("\"definitions\": [\n", out);
    for (size_t i = 0; i < c->members.count; i++) {
        ast_node_to_string(c->members.items[i].value, level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("],\n", out);

    /* functList, as Java's toString walks it. */
    const ast_class_tables *t = ast_class_tables_of(c);
    ast_node_write_tabs(level + 1, out);
    fputs("\"functionDefinitions\": [\n", out);
    for (size_t i = 0; i < t->funct_list.count; i++) {
        ast_node_to_string(&t->funct_list.items[i]->base, level + 2, out);
        fputs(",\n", out);
    }
    ast_node_write_tabs(level + 1, out);
    fputs("],\n", out);

    ast_node_write_tabs(level, out);
    fputc('}', out);
}

/* The implicit universal base. Tolerated as absent so an engine whose
 * base language failed to load still reports the real error rather
 * than a confusing missing-base one. */
#define OBJECT_BASE_NAME "object"

/* --- overload resolution (ports astClass.call's lookup) -----------
 * The hot path is one lookup of the call's "name#sig" key in the
 * dispatch map. Only when that misses, and the class declares the
 * name, are the slow paths tried, in order:
 *   1. promoted -- int->double substitutions; fewest promotions wins;
 *                  a tie is AMBIGUOUS_OVERLOAD
 *   2. wildcard -- untyped slots; most specific wins; tie -> ambiguous
 *   3. variadic -- the `...` overloads; most specific head wins
 * A key marked null-ambiguous that nothing matched is reported as such.
 * The promoted stage also converts the matched int args to doubles. */

/* astClass.tagMatches: the wildcard / variadic per-slot rule. cUndef
 * accepts anything, int widens to double, null fills a ref slot. */
static bool tag_match(c_type decl, c_type actual)
{
    if (decl == C_TYPE_UNDEF) return true;
    if (decl == actual) return true;
    if (decl == C_TYPE_DOUBLE && actual == C_TYPE_INT) return true;
    if (actual == C_TYPE_NULL) return ref_shape(decl);
    return false;
}

/* astClass.specificity: the slots other than ETCETERA whose effective
 * type is not cUndef. */
static int specificity(const ast_funct_def *fd)
{
    const ast_node_array *decl = ast_funct_def_args_list_args(fd->arg_list);
    int spec = 0;
    for (size_t i = 0; i < decl->count; i++) {
        if (decl->items[i]->type == AST_TYPE_ETCETERA) continue;
        if (ast_funct_def_effective_arg_type(decl->items[i]) != C_TYPE_UNDEF) spec++;
    }
    return spec;
}

/* astClass.overloadMatchesCall: the wildcard matcher. */
static bool overload_matches_call(const ast_funct_def *fd, av_list *args)
{
    int argc = (int)av_list_size(args);
    if (argc < ast_funct_def_min_arity(fd)) return false;
    if (argc > ast_funct_def_max_arity(fd)) return false;
    const ast_node_array *decl = ast_funct_def_args_list_args(fd->arg_list);
    for (int i = 0; i < argc; i++) {
        if (!tag_match(ast_funct_def_effective_arg_type(decl->items[i]),
                       av_list_get(args, (size_t)i)->type)) return false;
    }
    return true;
}

/* astClass.variadicMatches. */
static bool variadic_matches(const ast_funct_def *fd, av_list *args)
{
    const ast_node_array *decl = ast_funct_def_args_list_args(fd->arg_list);
    size_t head_len = decl->count - 1;
    size_t argc = av_list_size(args);
    if (argc < head_len) return false;
    for (size_t i = 0; i < head_len; i++) {
        if (!tag_match(ast_funct_def_effective_arg_type(decl->items[i]),
                       av_list_get(args, i)->type)) return false;
    }
    c_type etc_type = decl->items[decl->count - 1]->prim_type;
    if (etc_type != C_TYPE_UNDEF) {
        for (size_t i = head_len; i < argc; i++) {
            if (!tag_match(etc_type, av_list_get(args, i)->type)) return false;
        }
    }
    return true;
}

static av_list *build_promoted_args(av_list *args, unsigned long mask)
{
    av_list *out = av_list_new();
    size_t n = av_list_size(args);
    for (size_t i = 0; i < n; i++) {
        av_type *v = av_list_get(args, i);
        if ((mask & (1UL << i)) && v->type == C_TYPE_INT) {
            v = &av_double_new((double)av_int_get((av_int *)v))->base.base;
        }
        av_list_add(out, v);
    }
    return out;
}

typedef struct ovl_result {
    ast_funct_def *def;   /* chosen overload, or NULL */
    av_list *args;        /* possibly promoted arg list */
    av_type *error;       /* non-NULL on AMBIGUOUS_OVERLOAD */
} ovl_result;

/* astClass.mangleCallSig: the call's argument types, "i,d,s". */
static char *mangle_call_sig(av_list *args)
{
    size_t n = av_list_size(args);
    char *out = AUSSOM_ALLOC_ATOMIC(n * 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0) out[k++] = ',';
        out[k++] = ast_funct_def_mangle_char(av_list_get(args, i)->type);
    }
    out[k] = '\0';
    return out;
}

/* astClass.makeDispatchKey: "name#i,s". Written into `buf` when it
 * fits, otherwise into GC memory. */
static char *make_dispatch_key(const char *name, av_list *args, char *buf, size_t bufsz)
{
    size_t nlen = strlen(name);
    size_t argc = av_list_size(args);
    size_t need = nlen + 1 + argc * 2 + 1;
    char *key = (need <= bufsz) ? buf : AUSSOM_ALLOC_ATOMIC(need);
    memcpy(key, name, nlen);
    size_t k = nlen;
    key[k++] = '#';
    for (size_t i = 0; i < argc; i++) {
        if (i > 0) key[k++] = ',';
        key[k++] = ast_funct_def_mangle_char(av_list_get(args, i)->type);
    }
    key[k] = '\0';
    return key;
}

/* astClass.ambiguityError: "Call to '<class>' is ambiguous between:
 * f(i,d), f(d,i)". Java names the class here, not the function. */
static av_type *ambiguous_error(environment *env, ast_class *cls,
                                ast_funct_def **cands, size_t ncands)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) {
        return eval_make_exception(env, cls->base.line_num, "AMBIGUOUS_OVERLOAD",
                                   "Call is ambiguous.");
    }
    fprintf(fp, "Call to '%s' is ambiguous between: ", cls->base.name);
    for (size_t i = 0; i < ncands; i++) {
        if (i > 0) fputs(", ", fp);
        fprintf(fp, "%s(%s)", cands[i]->base.name, ast_funct_def_signature(cands[i]));
    }
    aussom_memstream_close(fp, &buf, &len);
    char *msg = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return eval_make_exception(env, cls->base.line_num, "AMBIGUOUS_OVERLOAD", msg);
}

/* astClass.matchPromoted: tries every int->double substitution of the
 * call's int arguments, fewest substitutions first, and takes the
 * unique match at the lowest count. Subsets of equal size are tried in
 * numeric order, as Java's stable sort leaves them. */
static ovl_result match_promoted(environment *env, ast_class *cls,
                                 const ast_class_tables *t, const char *name,
                                 av_list *args)
{
    ovl_result rr = { NULL, args, NULL };
    int argc = (int)av_list_size(args);
    if (argc == 0) return rr;
    int intpos[32];
    int nint = 0;
    for (int i = 0; i < argc && nint < 32; i++) {
        if (av_list_get(args, (size_t)i)->type == C_TYPE_INT) intpos[nint++] = i;
    }
    if (nint == 0 || nint > 20) return rr;

    size_t nlen = strlen(name);
    char *key = AUSSOM_ALLOC_ATOMIC(nlen + 1 + (size_t)argc * 2 + 1);
    funct_vec best = { NULL, 0, 0 };
    unsigned long best_mask = 0;
    unsigned long total = 1UL << nint;
    for (int pc = 1; pc <= nint && best.count == 0; pc++) {
        for (unsigned long sub = 1; sub < total; sub++) {
            if (__builtin_popcountl(sub) != pc) continue;
            unsigned long mask = 0;
            for (int j = 0; j < nint; j++) {
                if (sub & (1UL << j)) mask |= (1UL << intpos[j]);
            }
            memcpy(key, name, nlen);
            size_t k = nlen;
            key[k++] = '#';
            for (int i = 0; i < argc; i++) {
                if (i > 0) key[k++] = ',';
                key[k++] = (mask & (1UL << i)) ? 'd'
                    : ast_funct_def_mangle_char(av_list_get(args, (size_t)i)->type);
            }
            key[k] = '\0';
            ast_funct_def *hit = dispatch_get(t, key);
            if (hit == NULL) continue;
            if (best.count == 0) best_mask = mask;
            funct_vec_add(&best, hit);
        }
    }
    if (best.count == 1) {
        rr.def = best.items[0];
        rr.args = build_promoted_args(args, best_mask);
    } else if (best.count > 1) {
        rr.error = ambiguous_error(env, cls, best.items, best.count);
    }
    return rr;
}

/* astClass.matchWildcards: the most specific matching wildcard
 * overload; a tie is ambiguous. */
static ovl_result match_wildcards(environment *env, ast_class *cls,
                                  const ast_class_tables *t, const char *name,
                                  av_list *args)
{
    ovl_result rr = { NULL, args, NULL };
    int best_spec = -1;
    funct_vec best = { NULL, 0, 0 };
    for (size_t i = 0; i < t->wildcards.count; i++) {
        ast_funct_def *cand = t->wildcards.items[i];
        if (strcmp(cand->base.name, name) != 0) continue;
        if (!overload_matches_call(cand, args)) continue;
        int spec = specificity(cand);
        if (spec > best_spec) {
            best_spec = spec;
            best.count = 0;
            funct_vec_add(&best, cand);
        } else if (spec == best_spec) {
            funct_vec_add(&best, cand);
        }
    }
    if (best.count == 1) rr.def = best.items[0];
    else if (best.count > 1) rr.error = ambiguous_error(env, cls, best.items, best.count);
    return rr;
}

/* astClass.matchVariadic: with several matches, those with the highest
 * specificity whose declared length equals the first such overload's
 * are the candidates; more than one is ambiguous. */
static ovl_result match_variadic(environment *env, ast_class *cls,
                                 const ast_class_tables *t, const char *name,
                                 av_list *args)
{
    ovl_result rr = { NULL, args, NULL };
    funct_vec matches = { NULL, 0, 0 };
    for (size_t i = 0; i < t->variadics.count; i++) {
        ast_funct_def *fd = t->variadics.items[i];
        if (strcmp(fd->base.name, name) != 0) continue;
        if (!variadic_matches(fd, args)) continue;
        funct_vec_add(&matches, fd);
    }
    if (matches.count == 0) return rr;
    if (matches.count == 1) { rr.def = matches.items[0]; return rr; }

    /* Java sorts by descending specificity with a stable sort, so the
     * first overload at the top specificity sets the length. */
    int top_spec = -1;
    for (size_t i = 0; i < matches.count; i++) {
        int s = specificity(matches.items[i]);
        if (s > top_spec) top_spec = s;
    }
    size_t top_len = 0;
    bool have_len = false;
    funct_vec top = { NULL, 0, 0 };
    for (size_t i = 0; i < matches.count; i++) {
        ast_funct_def *fd = matches.items[i];
        if (specificity(fd) != top_spec) continue;
        size_t len = ast_funct_def_args_list_args(fd->arg_list)->count;
        if (!have_len) { top_len = len; have_len = true; }
        if (len == top_len) funct_vec_add(&top, fd);
    }
    if (top.count == 1) rr.def = top.items[0];
    else rr.error = ambiguous_error(env, cls, top.items, top.count);
    return rr;
}

static ovl_result resolve_call(environment *env, ast_class *cls,
                               const char *name, av_list *args)
{
    ovl_result rr = { NULL, args, NULL };
    const ast_class_tables *t = ast_class_tables_of(cls);
    char kbuf[128];
    char *key = make_dispatch_key(name, args, kbuf, sizeof(kbuf));
    rr.def = dispatch_get(t, key);
    if (rr.def != NULL) return rr;

    if (av_strmap_contains(t->declared_names, name)) {
        rr = match_promoted(env, cls, t, name, args);
        if (rr.error != NULL || rr.def != NULL) return rr;
        rr = match_wildcards(env, cls, t, name, args);
        if (rr.error != NULL || rr.def != NULL) return rr;
        rr = match_variadic(env, cls, t, name, args);
        if (rr.error != NULL || rr.def != NULL) return rr;
    }

    /* A null argument landed on a slot where two or more reference
     * typed overloads apply, and no wildcard or variadic overload took
     * it. That cannot be decided, so say so rather than pick one. */
    if (av_strmap_contains(t->null_ambiguous, key)) {
        char msg[320];
        snprintf(msg, sizeof(msg),
            "Ambiguous overload for '%s': a null argument matches more than "
            "one reference-typed overload and cannot be resolved. Add a "
            "wildcard overload (e.g. %s(x)) to accept null explicitly.",
            name, name);
        rr.error = eval_make_exception(env, cls->base.line_num,
            "AMBIGUOUS_OVERLOAD", msg);
    }
    return rr;   /* no match */
}

/* Walks the receiver class and its ancestors for the first class whose
 * bound extern class implements `name`. A bound class never changes
 * after it is registered, so this takes no lock. */
static extern_fn resolve_extern(engine *eng, ast_class *cls, const char *name,
                                ast_class **owner)
{
    extern_fn fn = NULL;
    const extern_class *ec = ast_class_get_bound_extern(cls);
    if (ec != NULL) {
        fn = extern_class_find_method(ec, name);
        if (fn != NULL) {
            *owner = cls;
            return fn;
        }
    }

    for (size_t i = 0; i < cls->extended_classes.count; i++) {
        ast_class *parent = engine_resolve_class(eng, cls->extended_classes.items[i]);
        if (parent != NULL) {
            fn = resolve_extern(eng, parent, name, owner);
            if (fn != NULL) return fn;
        }
    }
    return NULL;
}

/* The leaf value type a class ultimately binds, or C_TYPE_UNDEF for an
 * object-backed or plain class. Used to reject a value-type extern
 * method called on a plain object. A real primitive
 * carries its own tag, so only a confused object ever reaches an extern
 * with the wrong type. AussomObject is deliberately absent: its methods
 * (toJson, ...) are inherited by every value, whatever its tag. */
static c_type extern_value_ctype(engine *eng, ast_class *cls)
{
    const char *ref = ast_class_get_extern_class_name(cls);
    if (ref != NULL) {
        size_t n = strlen(ref);
#define AV_REF_ENDS(suf) (n >= sizeof(suf) - 1 && \
        strcmp(ref + n - (sizeof(suf) - 1), suf) == 0)
        if (AV_REF_ENDS("AussomString"))    return C_TYPE_STRING;
        if (AV_REF_ENDS("AussomInt"))       return C_TYPE_INT;
        if (AV_REF_ENDS("AussomDouble"))    return C_TYPE_DOUBLE;
        if (AV_REF_ENDS("AussomBool"))      return C_TYPE_BOOL;
        if (AV_REF_ENDS("AussomList"))      return C_TYPE_LIST;
        if (AV_REF_ENDS("AussomMap"))       return C_TYPE_MAP;
        if (AV_REF_ENDS("AussomNull"))      return C_TYPE_NULL;
        if (AV_REF_ENDS("AussomCallback"))  return C_TYPE_CALLBACK;
        if (AV_REF_ENDS("AussomException")) return C_TYPE_EXCEPTION;
#undef AV_REF_ENDS
    }
    for (size_t i = 0; i < cls->extended_classes.count; i++) {
        ast_class *parent = engine_resolve_class(eng, cls->extended_classes.items[i]);
        if (parent != NULL) {
            c_type t = extern_value_ctype(eng, parent);
            if (t != C_TYPE_UNDEF) return t;
        }
    }
    return C_TYPE_UNDEF;
}

/* The Java class that holds a value of type `t`, for the cast message
 * below. */
static const char *java_value_class(c_type t)
{
    switch (t) {
    case C_TYPE_NULL:      return "AussomNull";
    case C_TYPE_BOOL:      return "AussomBool";
    case C_TYPE_INT:       return "AussomInt";
    case C_TYPE_DOUBLE:    return "AussomDouble";
    case C_TYPE_STRING:    return "AussomString";
    case C_TYPE_LIST:      return "AussomList";
    case C_TYPE_MAP:       return "AussomMap";
    case C_TYPE_CALLBACK:  return "AussomCallback";
    case C_TYPE_EXCEPTION: return "AussomException";
    default:               return "AussomObject";
    }
}

/* What Java's no-argument constructor of the value class gives. */
static av_type *empty_value(c_type t)
{
    switch (t) {
    case C_TYPE_BOOL:      return &av_bool_new(false)->base.base;
    case C_TYPE_INT:       return &av_int_new(0)->base.base;
    case C_TYPE_DOUBLE:    return &av_double_new(0.0)->base.base;
    case C_TYPE_STRING:    return &av_string_new("")->base.base;
    case C_TYPE_LIST:      return &av_list_new()->base.base;
    case C_TYPE_MAP:       return &av_map_new()->base.base;
    case C_TYPE_CALLBACK:  return &av_callback_new(NULL, NULL, "")->base.base;
    case C_TYPE_EXCEPTION: return &av_exception_new_type(AV_EX_UNDEF)->base.base;
    default:               return &av_null_new()->base.base;
    }
}

/* Validates the arguments to an extern (C) method against the declared
 * parameter types before the C function sees them.
 *
 * Overload resolution already rejects a wrong non-null type for a typed
 * parameter (no overload matches, so the call never reaches here), but
 * its tag matcher lets `null` fill any reference-shaped slot. A Java
 * method then casts the argument, and the failed cast becomes
 * EXTERN_INVOCATION_TARGET_EXCEPTION (astFunctDef.callExtern). A C cast
 * cannot fail, so the same refusal is made here instead, unless that
 * parameter's default is itself null.

 *
 * A few stdlib methods take the null anyway, listed in null_ok below:
 * Date.between (ADate.between) checks isNull() before its cast, since
 * its `string Unit = "millis"` may be null and then means millis;
 * Lock.withLock checks instanceof before its cast; and the parse
 * methods of int, double, Int and Double catch the failed cast and
 * return their own parse exception.
 *
 * Untyped parameters (prim_type UNDEF) and the universal `object` type
 * are not checked here: the former carry no contract, and the latter
 * accepts any value, so the C function must validate those itself. */
static av_type *check_extern_args(environment *env, ast_funct_def *fd,
                                  av_list *args, ast_class *owner)
{
    /* Java methods that check a null argument themselves, before any
     * cast, or catch the failed cast: they get the null. */
    static const char *const null_ok[][2] = {
        { "com.aussom.stdlib.ADate", "between" },
        { "com.aussom.stdlib.ALock", "withLock" },
        { "com.aussom.types.AussomInt", "parse" },
        { "com.aussom.stdlib.SInt", "parse" },
        { "com.aussom.types.AussomDouble", "parse" },
        { "com.aussom.stdlib.SDouble", "parse" },
    };
    const char *ref = ast_class_get_extern_class_name(owner);
    bool takes_null = false;
    for (size_t k = 0; ref != NULL && k < sizeof(null_ok) / sizeof(null_ok[0]); k++) {
        if (strcmp(ref, null_ok[k][0]) == 0 && strcmp(fd->base.name, null_ok[k][1]) == 0)
            takes_null = true;
    }

    const ast_node_array *decl = ast_funct_def_args_list_args(fd->arg_list);
    size_t argc = av_list_size(args);
    for (size_t i = 0; i < decl->count; i++) {
        ast_node *adef = decl->items[i];
        if (adef->type == AST_TYPE_ETCETERA) break;   /* variadic: rest free */
        c_type want = adef->prim_type;
        if (want == C_TYPE_UNDEF || want == C_TYPE_OBJECT) continue;
        if (i >= argc) continue;                       /* defaulting handled elsewhere */
        av_type *a = av_list_get(args, i);
        if (a == NULL) continue;
        c_type got = a->type;
        if (got == want) continue;
        if (want == C_TYPE_DOUBLE && got == C_TYPE_INT) continue;  /* widening */
        if (got == C_TYPE_NULL
            && (takes_null || adef->type == AST_TYPE_NULL)) continue;
        char msg[320];
        snprintf(msg, sizeof(msg),
            "External call, invocation target exception for method '%s', "
            "the external method threw an uncaught exception: "
            "ClassCastException: %s cannot be cast to %s.",
            fd->base.name, java_value_class(got), java_value_class(want));
        return eval_make_exception(env, fd->base.line_num,
                                   "EXTERN_INVOCATION_TARGET_EXCEPTION", msg);
    }
    return NULL;
}

const char *eval_type_name(const av_type *v)
{
    if (v == NULL) return "undef";
    switch (v->type) {
    case C_TYPE_BOOL:      return "bool";
    case C_TYPE_INT:       return "int";
    case C_TYPE_DOUBLE:    return "double";
    case C_TYPE_STRING:    return "string";
    case C_TYPE_LIST:      return "list";
    case C_TYPE_MAP:       return "map";
    case C_TYPE_NULL:      return "cnull";
    case C_TYPE_CALLBACK:  return "callback";
    case C_TYPE_EXCEPTION: return "exception";
    default: {
        /* A user-defined or extern object: its name is the class it was
         * instantiated from. */
        const ast_class *cd = (v->type == C_TYPE_OBJECT)
            ? (const ast_class *)av_object_get_class_def((av_object *)v) : NULL;
        return (cd != NULL) ? cd->base.name : "object";
    }
    }
}

ast_class *class_def_for(environment *env, av_type *v)
{
    if (v == NULL) return NULL;
    /* Control carriers do not embed av_object, so the cast below would
     * not be valid for them. */
    if (v->type == C_TYPE_REF || v->type == C_TYPE_BREAK
        || v->type == C_TYPE_RETURN) {
        return NULL;
    }
    ast_class *def = (ast_class *)av_object_get_class_def((av_object *)v);
    if (def != NULL) return def;
    return engine_primitive_class_def(environment_engine(env), v->type);
}

/* --- public dispatch --------------------------------------------- */

av_type *class_call(environment *env, bool getref, ast_class *cls,
                    const char *name, av_list *args)
{
    return class_call_closure(env, getref, cls, name, args, NULL);
}

av_type *class_call_closure(environment *env, bool getref, ast_class *cls,
                            const char *name, av_list *args,
                            av_strmap *closure_parent_locals)
{
    engine *eng = environment_engine(env);

    /* Control and call depth, checked once here because every Aussom
     * call reaches the body through this function: direct calls,
     * constructors, callbacks, closures, operator overloads and extern
     * dispatch. A pause blocks, a cancel returns, and a call that would
     * nest deeper than the engine's limit is refused with a catchable
     * exception rather than running the native stack out. */
    av_type *gate = eval_check_call_gate(&cls->base, env, name);
    if (gate != NULL) return gate;

    av_type *cur = environment_cur_obj(env);
    av_object *cobj = (cur != NULL) ? (av_object *)cur : environment_class_instance(env);

    /* Test mock / spy interception, before normal dispatch (mirrors the
     * mock checks at the top of astClass.call). A simple/when mock
     * replaces the call; a spy records every call (real method runs). */
    bool spy_set = cobj != NULL && av_object_is_spy_set(cobj, name);
    if (spy_set && !security_manager_get_property_boolean(engine_get_security_manager(eng), "test.mock.spy", false)) {
        /* Java returns a bare AussomException: no id, no line. */
            return &av_exception_new(
            "astClass.call(): Security exception, action 'test.mock.spy' not permitted.")->base.base;
    }
    if (cobj != NULL && av_object_has_function_mock(cobj, name)) {
        if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "test.mock.inject", false)) {
            /* Java returns a bare AussomException: no id, no line. */
                return &av_exception_new(
                "astClass.call(): Security exception, action 'test.mock.inject' not permitted.")->base.base;
        }
        av_type *mret = av_object_process_mock(env, cobj, name, args);
        if (mret != NULL) {
            if (spy_set) av_object_add_spy_record(cobj, name, args, mret);
            return mret;
        }
    }

    ovl_result rr = resolve_call(env, cls, name, args);
    if (rr.error != NULL) return rr.error;
    ast_funct_def *fd = rr.def;
    av_list *use_args = rr.args;
    if (fd == NULL) {
        /* Fallback: a stored callback member with this name. */
        av_type *member = (cobj != NULL) ? av_object_get_member(cobj, name) : NULL;
        if (member != NULL) {
            if (member->type == C_TYPE_CALLBACK) {
                av_type *cret = callback_call((av_callback *)member, env, args);
                if (spy_set) av_object_add_spy_record(cobj, name, args, cret);
                return cret;
            }
        }
        char nf[512];
        snprintf(nf, sizeof(nf), "Object '%s' has no overload of '%s' matching call signature '%s'.",
                 cls->base.name, name, mangle_call_sig(args));
        return eval_make_exception(env, cls->base.line_num, "FUNCT_NOT_FOUND", nf);
    }

    /* Access control: an external caller (the calling instance is not
     * the receiver) cannot invoke a private overload. Mirrors
     * astClass.call. */
    if (cobj != NULL && environment_class_instance(env) != cobj
        && fd->base.access == ACCESS_PRIVATE) {
        char msg[160];
        snprintf(msg, sizeof(msg), "No access to function '%s' (private).", name);
        return eval_make_exception(env, fd->base.line_num, "NO_ACCESS", msg);
    }

    av_type *ret;
    if (ast_funct_def_get_extern(fd)) {
        /* Bridge to the C implementation in the bound extern class,
         * replacing Java reflection. `self` is the receiver value. */
        av_type *self = (cur != NULL) ? cur
                        : (cobj != NULL ? &cobj->base : NULL);
        /* A value-type extern reached with a plain object as the
         * receiver is a type confusion: a confused
         * `extern class ... : com.aussom.types.AussomString`, a
         * `new exception()`, and so on. Reject before the C cast. A
         * genuine primitive carries its own tag and skips this. */
        if (self != NULL && self->type == C_TYPE_OBJECT) {
            c_type vt = extern_value_ctype(eng, cls);
            /* astFunctDef.callExtern calls the method on the receiver's
             * extern object. For a value-backed class that is its value. */
            av_type *ev = av_object_get_extern_value((av_object *)self);
            if (ev != NULL && vt != C_TYPE_UNDEF && ev->type == vt) {
                self = ev;
                vt = C_TYPE_UNDEF;
            } else if (ev != NULL) {
                /* The extern object is not of the extern class (a failed
                 * parent constructor left an exception there): the
                 * reflective invoke throws IllegalArgumentException. */
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "External call, illegal argument exception for method '%s'.",
                    name);
                return eval_make_exception(env, fd->base.line_num,
                                           "EXTERN_ILLEGAL_ARGUMENT", msg);
            }
            if (vt != C_TYPE_UNDEF) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                    "Method '%s' expects a %s receiver but was called on "
                    "an object.", name, c_type_name(vt));
                return eval_make_exception(env, fd->base.line_num,
                                           "TYPE_ERROR", msg);
            }
        }
        ast_class *owner = cls;
        extern_fn fn = resolve_extern(eng, cls, name, &owner);
        if (fn == NULL) {
            /* The C counterpart of Java's NoSuchMethodException from
             * getMethod in astFunctDef.callExtern. */
            char msg[256];
            snprintf(msg, sizeof(msg), "External call, no such method '%s'.", name);
            return eval_make_exception(env, fd->base.line_num, "EXTERN_NO_SUCH_METHOD", msg);
        }
        /* Reject null (and any stray wrong type) in a typed slot before
         * the C function casts it. */
        av_type *argerr = check_extern_args(env, fd, use_args, owner);
        if (argerr != NULL) return argerr;
        /* A C function that walks a value (toJson, pack, clone,
         * printing) cannot return an exception from deep inside the
         * walk. When the walk runs the native stack low it marks the
         * thread instead, and the mark is turned into STACK_OVERFLOW
         * here. Cleared first so an earlier mark is not taken for one
         * from this call. See stack_guard.h. */
        stack_guard_clear();
        av_type *r = fn(self, env, use_args);
        if (stack_guard_tripped()) return eval_stack_overflow(env, fd->base.line_num);
        ret = (r != NULL) ? r : &av_null_new()->base.base;

        /* astFunctDef.callExtern: an exception the method built with
         * just a message gets the declaration's line, the call site's
         * stack trace and the id EXTERN_RUNTIME. One the method filled
         * in itself is left alone. */
        if (av_is_ex(ret)) {
            av_exception *ex = (av_exception *)ret;
            if (ex->thrown) {
                /* The method's Java version throws: the
                 * InvocationTargetException branch of callExtern. */
                const char *tail = av_exception_get_text(ex);
                size_t n = strlen(name) + strlen(tail) + 128;
                char *msg = AUSSOM_ALLOC_ATOMIC(n);
                snprintf(msg, n, "External call, invocation target exception for "
                         "method '%s', the external method threw an uncaught "
                         "exception: %s", name, tail);
                av_exception_set(ex, -1, "EXTERN_INVOCATION_TARGET_EXCEPTION",
                                 msg, msg, "");
                ex->thrown = false;
            }
            if (av_exception_get_line(ex) == -1)
                av_exception_set_line(ex, fd->base.line_num);
            const char *st = av_exception_get_stack_trace(ex);
            if (st == NULL || st[0] == '\0')
                av_exception_set_stack_trace(ex, environment_stack_trace(env));
            const char *id = av_exception_get_id(ex);
            if (id == NULL || id[0] == '\0')
                av_exception_set_id(ex, "EXTERN_RUNTIME");
        }
    } else {
        /* Fresh call frame: locals, ci = receiver object. Locals are
         * frame-private and never shared across threads, so the map
         * skips per-op locking. */
        av_strmap *locals = av_strmap_new_unlocked();

        /* A closure's locals start as a shallow copy of the parent
         * scope: the captured locals when invoked through a callback,
         * or the invoker's locals when called directly by name.
         * funct_def_call then layers the closure's own arguments on
         * top, shadowing copied names. Copying fresh on every
         * invocation keeps recursion and repeated calls isolated. */
        if (ast_funct_def_get_closure(fd)) {
            av_strmap *parent = (closure_parent_locals != NULL)
                                ? closure_parent_locals
                                : environment_locals(env);
            if (parent != NULL) {
                av_strmap_lock(parent);
                size_t pn = av_strmap_size_locked(parent);
                for (size_t i = 0; i < pn; i++) {
                    av_strmap_put(locals, av_strmap_key_at_locked(parent, i),
                                  av_strmap_value_at_locked(parent, i));
                }
                av_strmap_unlock(parent);
            }
        }

        /* The class name in this call's frames is the instance's type,
         * as env.getClassInstance().getTypeName() is in Java. */
        av_object *ci = (cobj != NULL) ? cobj : environment_class_instance(env);
        const char *ci_name = (ci != NULL) ? eval_type_name(&ci->base) : cls->base.name;

        /* astFunctDef.initArgs: arguments are bound, and defaults
         * evaluated, under a frame of their own on the caller's stack,
         * before the body's frame exists. The frame can only appear in
         * a trace when a default is evaluated or an argument is
         * missing, which needs fewer arguments than declared slots, so
         * it is only built then: this is the hottest path in the
         * interpreter. */
        environment *aenv = environment_new(eng);
        callstack *astack = environment_callstack(env);
        if (av_list_size(use_args)
            < ast_funct_def_args_list_args(fd->arg_list)->count) {
            size_t nlen = strlen(name);
            char *dname = AUSSOM_ALLOC_ATOMIC(nlen + sizeof(" <arg-defaults>"));
            memcpy(dname, name, nlen);
            memcpy(dname + nlen, " <arg-defaults>", sizeof(" <arg-defaults>"));
            astack = callstack_new_frame(fd->base.file_name, fd->base.line_num,
                                         ci_name, dname, "Default argument values.");
            callstack_set_parent(astack, environment_callstack(env));
            callstack_set_called_function(astack, fd);
        }
        environment_set(aenv, ci, locals, astack);
        av_type *bound = funct_def_init_args(aenv, fd, use_args);

        if (av_is_ex(bound)) {
            ret = bound;
        } else {
            /* astFunctDef.call's "Defined." frame. Java names the file
             * of the class the call resolved on and the line of the
             * definition. */
            callstack *cst = callstack_new_frame(cls->base.file_name, fd->base.line_num,
                                                 ci_name, name, "Defined.");
            callstack_set_parent(cst, environment_callstack(env));
            /* This is the frame that means "we are now one Aussom call
             * deeper", which is what the call depth limit counts. */
            callstack_enter_call(cst);
            callstack_set_called_function(cst, fd);

            environment *tenv = environment_new(eng);
            environment_set(tenv, ci, locals, cst);
            ret = funct_def_run_body(tenv, getref, fd);
        }
    }

    if (spy_set) av_object_add_spy_record(cobj, name, args, ret);
    return ret;
}

/* --- instantiation ----------------------------------------------- */

/* preserve_existing: when true, a member already present on the
 * instance is left untouched. Inherited members are applied with this
 * set so an already-applied (more-derived or earlier-listed) member
 * wins; the child's own members are applied with it false so the child
 * overwrites its parents. */
static av_type *instantiate_members_ex(environment *env, av_object *ci,
                                       ast_class *cls, bool preserve_existing)
{
    /* astClass.instantiateMembers: initializers run under a frame
     * naming the class whose members they are. */
    callstack *frame = callstack_new_frame(cls->base.file_name, cls->base.line_num,
                                           cls->base.name, "<member-init>",
                                           "Member initializers.");
    callstack_set_parent(frame, environment_callstack(env));
    environment *menv = environment_clone(env, NULL);
    environment_set_class_instance(menv, ci);
    environment_set_callstack(menv, frame);
    for (size_t i = 0; i < cls->members.count; i++) {
        if (preserve_existing
            && av_object_contains_member(ci, cls->members.items[i].name)) {
            continue;
        }
        ast_node *mv = cls->members.items[i].value;
        av_type *val = ast_node_eval(mv, menv, false);
        if (av_is_ex(val)) return val;
        av_object_add_member(ci, cls->members.items[i].name, val);
    }
    return NULL;   /* ok */
}

static av_type *instantiate_members(environment *env, av_object *ci, ast_class *cls)
{
    return instantiate_members_ex(env, ci, cls, false);
}

/* True for a parent that is genuinely backed by a host (C) extern type,
 * as opposed to the universal `object` root that every class inherits.
 * Only a genuine extern parent contributes an extern object, which is
 * what makes extern + regular multiple inheritance order independent:
 * a regular parent listed after an extern one must not overwrite the
 * extern backing. */
/* An error Java throws as an aussomException while it builds an object
 * (astClass.instantiate and the methods it calls). Java's `new`
 * (astNewInst) catches it and reports INSTANTIATION_FAILURE, with the
 * thrown message at the head of the stack trace; this builds that same
 * exception. ast_new_inst_eval then gives it the `new` expression's
 * line, as astNewInst does. */
av_type *class_instantiate_throw(environment *env, ast_class *cls, const char *msg)
{
    av_exception *e = av_exception_new_type(AV_EX_INTERNAL);
    char *trace = environment_stack_trace(env);
    size_t n = strlen(msg) + strlen(trace) + 2;
    char *full = AUSSOM_ALLOC_ATOMIC(n);
    snprintf(full, n, "%s\n%s", msg, trace);
    av_exception_set(e, cls->base.line_num, "INSTANTIATION_FAILURE",
                     CLASS_INSTANTIATION_THREW, CLASS_INSTANTIATION_THREW, full);
    return &e->base.base;
}

static bool is_real_extern_base(const ast_class *cls)
{
    if (!ast_class_get_extern(cls)) return false;
    const char *ref = ast_class_get_extern_class_name(cls);
    if (ref == NULL) return false;
    return strcmp(ref, "com.aussom.types.AussomObject") != 0;
}

/* astClass.instantiateInheritedClasses: applies each parent's members
 * to the new object, the parent's own before its ancestors', so a
 * more-derived class wins over its ancestors, and (with
 * preserve_existing) an earlier-listed parent wins over a later one. A
 * genuine extern parent also supplies the object's extern object. */
static av_type *instantiate_inherited(environment *env, av_object *ci, ast_class *cls,
                                      const ast_class_string_array *ext_classes)
{
    engine *eng = environment_engine(env);
    for (size_t i = 0; i < ext_classes->count; i++) {
        const char *ext = ext_classes->items[i];
        ast_class *parent = engine_resolve_class(eng, ext);
        if (parent == NULL) {
            /* Tolerate a missing "object" base, which only happens
             * when the base language did not load. */
            if (strcmp(ext, OBJECT_BASE_NAME) == 0) continue;
            char msg[320];
            snprintf(msg, sizeof(msg), "Extended class '%s' not found.", ext);
            return class_instantiate_throw(env, cls, msg);
        }

        if (is_real_extern_base(parent)) {
            av_type *pobj = class_instantiate(env, parent, false, av_list_new());
            if (pobj == NULL) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                    "Failed to instantiate class '%s', object is null.", ext);
                return class_instantiate_throw(env, cls, msg);
            }
            /* A parent constructor that fails (an error or a cancel)
             * fails the child too. */
            if (av_is_ex(pobj)) return pobj;
            /* cobj.setExternObject(ao.getExternObject()). An object
             * passes on its extern object. A value (`string`, `list`,
             * ...) is its own extern object in Java. */
            if (pobj->type == C_TYPE_OBJECT)
                av_object_set_extern_object(ci,
                    av_object_get_extern_object((av_object *)pobj));
            else
                av_object_set_extern_value(ci, pobj);
        }

        av_type *err = instantiate_members_ex(env, ci, parent, true);
        if (err != NULL) return err;
        if (parent->extended_classes.count > 0) {
            err = instantiate_inherited(env, ci, cls, &parent->extended_classes);
            if (err != NULL) return err;
        }
    }
    return NULL;
}

/* One lock for every class's init, as Java's per-class monitor. It is
 * recursive because a class's init runs its parents' inits. Init only
 * happens once per class, so one lock costs nothing that matters. */
static pthread_mutex_t g_init_lock;
static pthread_once_t g_init_lock_once = PTHREAD_ONCE_INIT;

static void init_lock_create(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_init_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static av_type *class_init(environment *env, ast_class *cls);

/* astClass.mergeInherited: adds the parents' dispatch entries,
 * wildcard and variadic overloads and members to `t`, the class's
 * table being built. Each parent is initialized first, and its own
 * parents are merged before its entries are, so an entry the class
 * does not shadow comes from the first class in that walk that has it.
 * Child entries shadow parent ones: by dispatch key, by name and
 * signature for a wildcard overload, and by name and head signature
 * for a variadic one. */
static av_type *merge_inherited(environment *env, ast_class *cls, ast_class_tables *t,
                                const ast_class_string_array *ext_classes)
{
    engine *eng = environment_engine(env);
    for (size_t i = 0; i < ext_classes->count; i++) {
        const char *ext = ext_classes->items[i];
        ast_class *ac = engine_resolve_class(eng, ext);
        if (ac == NULL) {
            if (strcmp(ext, OBJECT_BASE_NAME) == 0) continue;
            char msg[320];
            snprintf(msg, sizeof(msg), "Extended class '%s' not found.", ext);
            return class_instantiate_throw(env, cls, msg);
        }
        av_type *err = class_init(env, ac);
        if (err != NULL) return err;

        if (!string_array_contains(&t->all_extended, ac->base.name))
            string_array_add(&t->all_extended, ac->base.name);

        if (ac->extended_classes.count > 0) {
            err = merge_inherited(env, cls, t, &ac->extended_classes);
            if (err != NULL) return err;
        }

        const ast_class_tables *pt = ast_class_tables_of(ac);
        for (size_t k = 0; k < pt->dispatch->order_count; k++) {
            const char *key = av_strmap_key_at_locked(pt->dispatch, k);
            if (av_strmap_contains(t->dispatch, key)) continue;
            ast_funct_def *fd = (ast_funct_def *)av_strmap_value_at_locked(pt->dispatch, k);
            av_strmap_put(t->dispatch, key, (av_type *)fd);
            if (!funct_vec_contains(&t->funct_list, fd)) funct_vec_add(&t->funct_list, fd);
            av_strmap_put(t->declared_names, fd->base.name, SET_MARK);
        }
        /* Inherit the null-ambiguous keys, so a child reports the same
         * ambiguity, unless the child has its own overload at the key. */
        for (size_t k = 0; k < pt->null_ambiguous->order_count; k++) {
            const char *key = av_strmap_key_at_locked(pt->null_ambiguous, k);
            if (!av_strmap_contains(t->dispatch, key))
                av_strmap_put(t->null_ambiguous, key, SET_MARK);
        }
        for (size_t k = 0; k < pt->wildcards.count; k++) {
            ast_funct_def *pdef = pt->wildcards.items[k];
            const char *psig = ast_funct_def_signature(pdef);
            bool shadowed = false;
            for (size_t c = 0; c < t->wildcards.count && !shadowed; c++) {
                ast_funct_def *cdef = t->wildcards.items[c];
                shadowed = strcmp(cdef->base.name, pdef->base.name) == 0
                           && strcmp(ast_funct_def_signature(cdef), psig) == 0;
            }
            if (shadowed) continue;
            funct_vec_add(&t->wildcards, pdef);
            if (!funct_vec_contains(&t->funct_list, pdef)) funct_vec_add(&t->funct_list, pdef);
            av_strmap_put(t->declared_names, pdef->base.name, SET_MARK);
        }
        for (size_t k = 0; k < pt->variadics.count; k++) {
            ast_funct_def *pdef = pt->variadics.items[k];
            const char *phead = head_signature(ast_funct_def_signature(pdef));
            bool shadowed = false;
            for (size_t c = 0; c < t->variadics.count && !shadowed; c++) {
                ast_funct_def *cdef = t->variadics.items[c];
                shadowed = strcmp(cdef->base.name, pdef->base.name) == 0
                           && strcmp(head_signature(ast_funct_def_signature(cdef)), phead) == 0;
            }
            if (shadowed) continue;
            funct_vec_add(&t->variadics, pdef);
            if (!funct_vec_contains(&t->funct_list, pdef)) funct_vec_add(&t->funct_list, pdef);
            av_strmap_put(t->declared_names, pdef->base.name, SET_MARK);
        }

        /* Inherited members go into the lookup map only, so access
         * checks can find an inherited member's access word. */
        for (size_t k = 0; k < pt->memb_defs->order_count; k++) {
            const char *mname = av_strmap_key_at_locked(pt->memb_defs, k);
            if (!av_strmap_contains(t->memb_defs, mname))
                av_strmap_put(t->memb_defs, mname, av_strmap_value_at_locked(pt->memb_defs, k));
        }
    }
    return NULL;
}

/* astClass.findExtern: the class holding the real extern backing, in
 * `cls` or its ancestors, or NULL when there is none other than the
 * object base. Walks the declared parents rather than relying on init,
 * so the answer does not depend on which classes have already been
 * initialized. A class that extends itself, directly or not, would
 * recurse until the native stack ran out; *overflow is set instead
 * (Java ends in a StackOverflowError there). */
static ast_class *find_extern(engine *eng, ast_class *cls, bool *overflow)
{
    if (stack_guard_low()) { *overflow = true; return NULL; }
    if (is_real_extern_base(cls)) return cls;
    for (size_t i = 0; i < cls->extended_classes.count; i++) {
        ast_class *ac = engine_resolve_class(eng, cls->extended_classes.items[i]);
        if (ac != NULL) {
            ast_class *ext = find_extern(eng, ac, overflow);
            if (ext != NULL || *overflow) return ext;
        }
    }
    return NULL;
}

/* astClass.initSync, run under the init lock. */
static av_type *class_init_sync(environment *env, ast_class *cls)
{
    engine *eng = environment_engine(env);

    /* A class that extends itself, directly or not, recurses here
     * until the native stack runs low. */
    if (stack_guard_low()) return eval_stack_overflow(env, cls->base.line_num);

    bool found_extern = cls->is_extern;
    for (size_t i = 0; i < cls->extended_classes.count; i++) {
        const char *ext = cls->extended_classes.items[i];
        ast_class *ac = engine_resolve_class(eng, ext);
        if (ac == NULL) {
            if (strcmp(ext, OBJECT_BASE_NAME) == 0) continue;
            char msg[320];
            snprintf(msg, sizeof(msg), "Extended class '%s' not found.", ext);
            return class_instantiate_throw(env, cls, msg);
        }

        /* Static classes are startup singletons and cannot serve as a
         * base: extending one would produce an ordinary, independently
         * instantiable class that no longer shares the singleton's
         * state. */
        if (ast_class_get_static(ac)) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                "Cannot extend static class '%s'. Static classes cannot be "
                "used as a base class.", ext);
            return class_instantiate_throw(env, cls, msg);
        }

        /* The parent may not be initialized yet, so its own extern
         * fields cannot be trusted. Search it and its ancestors for a
         * real extern class. */
        bool overflow = false;
        ast_class *xc = find_extern(eng, ac, &overflow);
        if (overflow) return eval_stack_overflow(env, cls->base.line_num);
        if (xc != NULL) {
            if (found_extern) {
                char msg[384];
                snprintf(msg, sizeof(msg),
                    "Cannot inherit from two external classes. First is '%s' "
                    "and second is '%s'.", cls->extern_class_name, ext);
                return class_instantiate_throw(env, cls, msg);
            }
            found_extern = true;
            cls->is_extern = true;
            cls->extern_class_name = xc->extern_class_name;
        } else if (ac->is_extern) {
            /* A parent backed only by the universal object base is not
             * a real extern class and does not count toward the one
             * extern parent limit. It does make the class extern, with
             * that backing, when the class has none of its own. */
            if (cls->extern_class_name[0] == '\0') {
                cls->is_extern = true;
                cls->extern_class_name = ac->extern_class_name;
            }
        }
    }

    /* The merge builds a new table, which is then published whole, so
     * a thread reading the old one never sees it half changed. */
    ast_class_tables *t = tables_copy(ast_class_tables_of(cls));
    av_type *err = merge_inherited(env, cls, t, &cls->extended_classes);
    if (err != NULL) return err;
    __atomic_store_n(&cls->tables, t, __ATOMIC_RELEASE);
    __atomic_store_n(&cls->init_ran, true, __ATOMIC_RELEASE);
    return NULL;
}

/* astClass.init: runs initSync the first time, at the class's first
 * instantiation or through a class extending it. Returns the exception
 * an init error raises, or NULL. */
static av_type *class_init(environment *env, ast_class *cls)
{
    if (__atomic_load_n(&cls->init_ran, __ATOMIC_ACQUIRE)) return NULL;
    pthread_once(&g_init_lock_once, init_lock_create);
    pthread_mutex_lock(&g_init_lock);
    av_type *err = NULL;
    if (!cls->init_ran) err = class_init_sync(env, cls);
    pthread_mutex_unlock(&g_init_lock);
    return err;
}

av_type *class_instantiate(environment *env, ast_class *cls, bool getref,
                           av_list *args)
{
    /* A static class is a singleton: the engine builds its one and only
     * object at startup and every reference to the class name resolves
     * to that object. Building another here would hand back a detached
     * copy of the members that silently drifts from the singleton, so
     * every caller other than the startup path is refused. */
    if (ast_class_get_static(cls)) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "Cannot instantiate static class '%s'. Static classes are "
            "singletons and are accessed by class name.", cls->base.name);
        return eval_make_exception(env, cls->base.line_num,
            "STATIC_INSTANTIATION", msg);
    }
    return class_instantiate_static_singleton(env, cls, getref, args);
}

av_type *class_instantiate_static_singleton(environment *env, ast_class *cls,
                                            bool getref, av_list *args)
{
    /* astClass.instantiateImpl: "First run init if it hasn't been ran
     * for the class." */
    av_type *ierr = class_init(env, cls);
    if (ierr != NULL) return ierr;

    /* Core value types: `new list()` / `new map()` / `new string()` /
     * `new int()` ... must produce the actual av value (with its backing
     * storage and the right c_type tag), not a generic object -- otherwise
     * their methods (list.add, ...) operate on a NULL payload. Mirrors Java,
     * where these classes ARE the value types. */
    const char *cn = cls->base.name;
    if (strcmp(cn, "list") == 0)   return &av_list_new()->base.base;
    if (strcmp(cn, "map") == 0)    return &av_map_new()->base.base;
    if (strcmp(cn, "string") == 0) return &av_string_new("")->base.base;
    if (strcmp(cn, "int") == 0)    return &av_int_new(0)->base.base;
    if (strcmp(cn, "double") == 0) return &av_double_new(0.0)->base.base;
    if (strcmp(cn, "bool") == 0)   return &av_bool_new(false)->base.base;

    av_object *ci = av_object_new();
    av_object_set_class_def(ci, (struct ast_class *)cls);
    av_object_set_extern_object(ci, ci);
    /* astClass.instantiateExtern: a class whose extern class is a value
     * type (`class s : string`) starts with an empty value of that type
     * as its extern object, as externClass.newInstance() gives Java. */
    c_type vt = extern_value_ctype(environment_engine(env), cls);
    if (vt != C_TYPE_UNDEF) av_object_set_extern_value(ci, empty_value(vt));

    av_type *err = instantiate_inherited(env, ci, cls, &cls->extended_classes);
    if (err != NULL) return err;
    err = instantiate_members(env, ci, cls);
    if (err != NULL) return err;

    /* Constructor: a method named after the class. */
    if (ast_class_has_any_function_by_name(cls, cls->base.name)) {
        /* astClass.instantiate's "Constructor called." frame, at the
         * class definition, on top of the caller's stack. */
        callstack *cst = callstack_new_frame(cls->base.file_name, cls->base.line_num,
                                             cls->base.name, cls->base.name,
                                             "Constructor called.");
        callstack_set_parent(cst, environment_callstack(env));
        environment *tcenv = environment_clone(env, &ci->base);
        environment_set_class_instance(tcenv, ci);
        environment_set_callstack(tcenv, cst);
        av_type *cret = class_call(tcenv, getref, cls, cls->base.name, args);
        if (av_is_ex(cret)) return cret;
    }
    return &ci->base;
}

