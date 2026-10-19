/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `reflect` class (com.aussom.stdlib.AReflect) and the
 * `RClass` extern class (com.aussom.stdlib.AClass) that wraps a parsed
 * class definition for introspection.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "aussom/ast_annotation.h"
#include "aussom/ast_class.h"
#include "aussom/ast_eval.h"
#include "aussom/av.h"
#include "aussom/av_object.h"
#include "aussom/engine.h"
#include "aussom/security_manager.h"
#include "aussom/extern_registry.h"
#include "aussom/parser.h"
#include "aussom/stdlib_sources.h"
#include "stdlib_internal.h"

static av_type *rx(const char *m) { return &av_exception_new(m)->base.base; }

/* An RClass instance stores one of these as its extern_object. The tag
 * lets rclass_of reject an object that is not a genuine RClass made by
 * reflect.getClassDef. First field so the read is always
 * in bounds. */
#define RCLASS_MAGIC 0x52434c53u   /* "RCLS" */
typedef struct rclass_box { uint32_t magic; ast_class *cls; } rclass_box;

/* --- reflect ----------------------------------------------------- */

static av_type *reflect_instantiate(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *name = av_list_get_string(args, 0);
    if (name == NULL) return rx("reflect_instantiate: argument 1 must be a string.");
    ast_class *cls = engine_resolve_class(environment_engine(env), name);
    /* Reflection must not be a back door around the `new` restriction:
     * a static class is a singleton reached by class name. */
    if (cls != NULL && ast_class_get_static(cls)) {
        size_t n = strlen(name) + 128;
        char *m = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(m, n, "reflect.instantiate(): Cannot instantiate static class '%s'. "
                 "Static classes are singletons and are accessed by class name.", name);
        return rx(m);
    }
    /* Engine.instantiateObject throws for a missing class and for a
     * failed instantiation; AReflect reports both the same way (two
     * spaces and the trailing newline are Java's). */
    av_type *r = (cls != NULL) ? class_instantiate(env, cls, false, av_list_new()) : NULL;
    if (r == NULL || av_is_ex(r)) {
        size_t n = strlen(name) + 64;
        char *m = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(m, n, "reflect.instantiate(): Failed to instantiate class  '%s'.\n", name);
        return rx(m);
    }
    return r;
}

static av_type *reflect_classExists(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *name = av_list_get_string(args, 0);
    if (name == NULL) return rx("reflect_classExists: argument 1 must be a string.");
    bool ex = engine_resolve_class(environment_engine(env), name) != NULL;
    return &av_bool_new(ex)->base.base;
}

static av_type *reflect_loadedClasses(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)args;
    engine *eng = environment_engine(env);
    av_list *out = av_list_new();
    size_t n = engine_class_count(eng);
    for (size_t i = 0; i < n; i++) {
        av_list_add(out, &av_string_new(engine_get_class(eng, i)->base.name)->base.base);
    }
    return &out->base.base;
}

static av_type *reflect_loadedModules(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)args;
    engine *eng = environment_engine(env);
    av_list *out = av_list_new();
    size_t n = engine_include_count(eng);
    for (size_t i = 0; i < n; i++) {
        av_list_add(out, &av_string_new(engine_get_include(eng, i))->base.base);
    }
    return &out->base.base;
}

static av_type *reflect_isModuleLoaded(av_type *self, environment *env, av_list *args)
{
    (void)self;
    const char *name = av_list_get_string(args, 0);
    if (name == NULL) return rx("reflect_isModuleLoaded: argument 1 must be a string.");
    /* AReflect.isModuleLoaded: the module name with '.' as '/' and
     * ".aus" added, as the include list holds it. */
    size_t nl = strlen(name);
    char *inc = AUSSOM_ALLOC_ATOMIC(nl + 5);
    for (size_t i = 0; i < nl; i++) inc[i] = (name[i] == '.') ? '/' : name[i];
    memcpy(inc + nl, ".aus", 5);
    engine *eng = environment_engine(env);
    size_t n = engine_include_count(eng);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(engine_get_include(eng, i), inc) == 0) return &av_bool_new(true)->base.base;
    }
    return &av_bool_new(false)->base.base;
}

static av_type *reflect_evalStr(av_type *self, environment *env, av_list *args)
{
    (void)self;
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "reflect.eval.string", false)) {
        return rx("reflect.evalStr(): Security exception, action 'reflect.eval.string' not permitted.");
    }
    const char *code = av_list_get_string(args, 0);
    if (code == NULL) return rx("reflect_evalStr: argument 1 must be a string.");
    const char *name = (av_list_size(args) > 1 && av_list_get(args, 1)->type == C_TYPE_STRING)
        ? av_string_get((av_string *)av_list_get(args, 1)) : "evalStr";

    /* The flag is engine-wide and sticky, so clear it first and observe
     * only what this parse sets. Clear it again before returning the
     * failure: leaving it set would poison every later parse on this
     * engine. Diagnostics are deliberately left in place so
     * reflect.parseDiagnostics() can report the position. */
    engine_clear_parse_error(eng);
    aussom_parse_string(eng, code, name);
    /* An error Java's parse throws is caught and returned with its
     * message, ahead of the parse-error flag check. */
    const char *thrown = engine_parse_exception(eng);
    if (thrown != NULL) return rx(thrown);
    if (engine_has_parse_error(eng)) {
        engine_clear_parse_error(eng);
        char msg[224];
        snprintf(msg, sizeof(msg),
            "reflect.evalStr(): Parse error in evaluated string '%s'. Call "
            "reflect.parseDiagnostics() for the position and message.", name);
        return rx(msg);
    }
    /* AReflect.evalStr returns the calling instance. */
    av_object *ci = environment_class_instance(env);
    return (ci != NULL) ? &ci->base : &av_null_new()->base.base;
}

static av_type *reflect_parseDiagnostics(av_type *self, environment *env, av_list *args)
{
    (void)self; (void)args;
    engine *eng = environment_engine(env);
    av_list *out = av_list_new();
    size_t n = engine_parse_diagnostic_count(eng);
    for (size_t i = 0; i < n; i++) {
        const parse_diagnostic *d = engine_get_parse_diagnostic(eng, i);
        av_map *m = av_map_new();
        av_map_put(m, "file", &av_string_new(d->file_name)->base.base);
        av_map_put(m, "line", &av_int_new(d->line)->base.base);
        av_map_put(m, "col", &av_int_new(d->col)->base.base);
        av_map_put(m, "severity", &av_string_new(d->severity)->base.base);
        av_map_put(m, "message", &av_string_new(d->message)->base.base);
        av_list_add(out, &m->base.base);
    }
    return &out->base.base;
}

static av_type *reflect_clearParseDiagnostics(av_type *self, environment *env, av_list *args)
{
    (void)args;
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "reflect.clear.diagnostics", false)) {
        return rx("reflect.clearParseDiagnostics(): Security exception, action "
                  "'reflect.clear.diagnostics' not permitted.");
    }
    engine_clear_parse_diagnostics(eng);
    return self;
}

static av_type *reflect_evalFile(av_type *self, environment *env, av_list *args)
{
    (void)self;
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "reflect.eval.file", false)) {
        return rx("reflect.evalFile(): Security exception, action 'reflect.eval.file' not permitted.");
    }
    const char *file = av_list_get_string(args, 0);
    if (file == NULL) return rx("reflect_evalFile: argument 1 must be a string.");
    /* AReflect.evalFile returns what Engine.parseFile throws, as an
     * exception with its message. A syntax error is not thrown, so it
     * is logged by the parse and the call still returns null. */
    aussom_parse_file(eng, file);
    const char *thrown = engine_parse_exception(eng);
    if (thrown != NULL) return rx(thrown);
    return &av_null_new()->base.base;
}

/* Ports AReflect.includeModule: parses the file the dotted module name
 * names, relative to the working directory ("acme.net" ->
 * "acme/net.aus"), through Engine.parseFile. Not an include: the
 * engine's modules and include paths are not consulted, and nothing is
 * recorded in the include list.
 *
 * What parseFile throws (the file can't be read, it is over the source
 * byte limit, or a parse error Java throws) is returned as an exception
 * with its message. Syntax errors in the file are reported by the
 * parse, as in Java, and the call still returns the instance. */
static av_type *reflect_includeModule(av_type *self, environment *env, av_list *args)
{
    (void)self;
    engine *eng = environment_engine(env);
    if (!security_manager_get_property_boolean(engine_get_security_manager(eng), "reflect.include.module", false)) {
        return rx("reflect.includeModule(): Security exception, action 'reflect.include.module' not permitted.");
    }
    const char *mod = av_list_get_string(args, 0);
    if (mod == NULL) return rx("reflect_includeModule: argument 1 must be a string.");
    char path[PATH_MAX];
    size_t n = strlen(mod);
    if (n + sizeof(".aus") > sizeof(path)) {
        return rx("reflect.includeModule(): Module name is too long.");
    }
    for (size_t i = 0; i < n; i++) path[i] = (mod[i] == '.') ? '/' : mod[i];
    memcpy(path + n, ".aus", sizeof(".aus"));

    aussom_parse_file(eng, path);
    const char *thrown = engine_parse_exception(eng);
    if (thrown != NULL) return rx(thrown);
    av_object *ci = environment_class_instance(env);
    return (ci != NULL) ? &ci->base : &av_null_new()->base.base;
}

static av_type *reflect_invoke(av_type *self, environment *env, av_list *args)
{
    (void)self;
    av_type *obj = av_list_get(args, 0);
    const char *mname = av_list_get_string(args, 1);
    if (mname == NULL) return rx("reflect_invoke: argument 2 must be a string.");
    av_list *cargs = av_list_new();
    for (size_t i = 2; i < av_list_size(args); i++) av_list_add(cargs, av_list_get(args, i));
    ast_class *cls = class_def_for(env, obj);
    if (cls == NULL) return rx("reflect.invoke(): receiver has no class definition.");
    environment *tenv = environment_clone(env, obj);
    return class_call(tenv, false, cls, mname, cargs);
}

static av_type *reflect_getClassDef(av_type *self, environment *env, av_list *args)
{
    (void)self;
    engine *eng = environment_engine(env);
    const char *name = av_list_get_string(args, 0);
    if (name == NULL) return rx("reflect_getClassDef: argument 1 must be a string.");
    ast_class *target = engine_resolve_class(eng, name);
    if (target == NULL) {
        size_t n = strlen(name) + 48;
        char *m = AUSSOM_ALLOC_ATOMIC(n);
        snprintf(m, n, "reflect.getClassDef(): Class '%s' not found.", name);
        return rx(m);
    }
    ast_class *rcls = engine_resolve_class(eng, "RClass");
    if (rcls == NULL) return rx("reflect.getClassDef(): Class 'RClass' not found.");
    av_type *inst = class_instantiate(env, rcls, false, av_list_new());
    if (av_is_ex(inst)) return inst;
    /* Wrap the target class in a tagged box so rclass_of can tell a
     * genuine RClass (made here) from a `new RClass()` or a subclass of
     * RClass, whose extern_object is the instance or a copied parent
     * object rather than a class pointer. */
    rclass_box *box = AUSSOM_ALLOC(sizeof(rclass_box));
    box->magic = RCLASS_MAGIC;
    box->cls = target;
    av_object_set_extern_object((av_object *)inst, box);
    return inst;
}

/* --- RClass (wraps an ast_class * in a tagged box) --------------- */

static ast_class *rclass_of(av_type *self)
{
    if (self == NULL || self->type != C_TYPE_OBJECT) return NULL;
    void *eo = av_object_get_extern_object((av_object *)self);
    if (eo == NULL || eo == (void *)self) return NULL;
    if (((rclass_box *)eo)->magic != RCLASS_MAGIC) return NULL;
    return ((rclass_box *)eo)->cls;
}

/* An RClass not made by reflect.getClassDef (a `new RClass()`) has no
 * class: AClass's methods throw a NullPointerException on
 * this.classDef. `call` names the astClass method Java reaches. */
static av_type *rclass_badself(const char *call)
{
    char m[192];
    snprintf(m, sizeof(m), "java.lang.NullPointerException: Cannot invoke "
             "\"com.aussom.ast.astClass.%s()\" because \"this.classDef\" is null", call);
    return &av_exception_new_thrown(m)->base.base;
}

static av_type *rclass_getName(av_type *self, environment *env, av_list *args)
{ (void)env; (void)args; ast_class *c = rclass_of(self);
  if (c == NULL) return rclass_badself("getName");
  return &av_string_new(c->base.name)->base.base; }

static av_type *rclass_isStatic(av_type *self, environment *env, av_list *args)
{ (void)env; (void)args; ast_class *c = rclass_of(self);
  if (c == NULL) return rclass_badself("getStatic");
  return &av_bool_new(ast_class_get_static(c))->base.base; }

static av_type *rclass_isExtern(av_type *self, environment *env, av_list *args)
{ (void)env; (void)args; ast_class *c = rclass_of(self);
  if (c == NULL) return rclass_badself("getExtern");
  return &av_bool_new(ast_class_get_extern(c))->base.base; }

static av_type *rclass_getExternClassName(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    ast_class *c = rclass_of(self);
    if (c == NULL) return rclass_badself("getExternClassName");
    const char *n = ast_class_get_extern_class_name(c);
    return &av_string_new(n != NULL ? n : "")->base.base;
}

/* A c_type as Java's getType().name().toLowerCase().substring(1)
 * gives it: "int", "string", "null", "callback", ... */
static av_type *type_short_name(c_type t)
{
    const char *cn = c_type_name(t);
    size_t len = strlen(cn);
    char *out = AUSSOM_ALLOC_ATOMIC(len);
    for (size_t i = 1; i <= len; i++) out[i - 1] = (char)tolower((unsigned char)cn[i]);
    return &av_string_new(out)->base.base;
}

/* astAnnotation.getAussomType for each annotation on `n`. */
static av_list *annotation_list(const ast_node *n)
{
    av_list *l = av_list_new();
    for (size_t i = 0; i < n->annotations.count; i++) {
        const ast_annotation *an = (const ast_annotation *)n->annotations.items[i];
        av_map *am = av_map_new();
        av_map_put(am, "annotationName",
                   &av_string_new(ast_annotation_get_name(an))->base.base);
        av_map *args = av_map_new();
        for (size_t k = 0; k < an->args.count; k++) {
            const ast_annotation_arg *aa = (const ast_annotation_arg *)an->args.items[k];
            av_map_put(args, ast_annotation_arg_get_key(aa),
                       &av_string_new(ast_annotation_arg_get_value(aa))->base.base);
        }
        av_map_put(am, "annotationArgs", &args->base.base);
        av_list_add(l, &am->base.base);
    }
    return l;
}

static av_type *rclass_getMembers(av_type *self, environment *env, av_list *args)
{
    (void)args;
    ast_class *cls = rclass_of(self);
    if (cls == NULL) return rclass_badself("getMembers");
    const char **names;
    ast_node **decls;
    size_t n = ast_class_get_members(cls, &names, &decls);
    av_map *m = av_map_new();
    for (size_t i = 0; i < n; i++) {
        av_map *md = av_map_new();
        av_map_put(md, "type", &av_string_new(ast_node_type_name(decls[i]->type))->base.base);
        av_map_put(md, "annotations", &annotation_list(decls[i])->base.base);
        av_map_put(m, names[i], &md->base.base);
    }
    return &m->base.base;
}

static av_type *rclass_getMethods(av_type *self, environment *env, av_list *args)
{
    (void)args;
    ast_class *cls = rclass_of(self);
    if (cls == NULL) return rclass_badself("getFileName");
    engine *eng = environment_engine(env);

    /* Default values are evaluated under a frame of their own, so a
     * trace or debugger pause inside one shows the class under
     * reflection. */
    callstack *rframe = callstack_new_frame(cls->base.file_name, cls->base.line_num,
                                            cls->base.name, "<reflect.getMethods>",
                                            "Reflection: default argument values.");
    callstack_set_parent(rframe, environment_callstack(env));
    environment *tenv = environment_new(eng);
    environment_set(tenv, environment_class_instance(env), environment_locals(env), rframe);

    size_t nf;
    ast_funct_def **fns = ast_class_get_all_functions(cls, &nf);
    av_map *m = av_map_new();
    for (size_t i = 0; i < nf; i++) {
        ast_funct_def *fd = fns[i];
        av_map *fm = av_map_new();
        av_map_put(fm, "isExtern", &av_bool_new(ast_funct_def_get_extern(fd))->base.base);
        av_map_put(fm, "signature", &av_string_new(ast_funct_def_signature(fd))->base.base);
        av_map_put(fm, "annotations", &annotation_list(&fd->base)->base.base);

        av_list *alist = av_list_new();
        const ast_node_array *decl = ast_funct_def_args_list_args(fd->arg_list);
        for (size_t k = 0; k < decl->count; k++) {
            ast_node *tn = decl->items[k];
            av_map *am = av_map_new();
            if (tn->type == AST_TYPE_ETCETERA) {
                av_map_put(am, "name", &av_string_new("...")->base.base);
            } else {
                av_map_put(am, "name", &av_string_new(tn->name)->base.base);
                av_map_put(am, "requiredType", tn->prim_type != C_TYPE_UNDEF
                           ? type_short_name(tn->prim_type)
                           : &av_string_new("")->base.base);
                if (tn->type == AST_TYPE_VAR) {
                    av_map_put(am, "hasDefaultValue", &av_bool_new(false)->base.base);
                } else {
                    av_map_put(am, "hasDefaultValue", &av_bool_new(true)->base.base);
                    av_type *dv = ast_node_eval(tn, tenv, false);
                    av_map_put(am, "defaultValueType", type_short_name(dv->type));
                    av_map_put(am, "defaultValue", &av_string_new(av_str(dv))->base.base);
                }
            }
            av_list_add(alist, &am->base.base);
        }
        av_map_put(fm, "arguments", &alist->base.base);

        av_type *ov = av_map_get(m, fd->base.name);
        if (ov == NULL) {
            ov = &av_list_new()->base.base;
            av_map_put(m, fd->base.name, ov);
        }
        av_list_add((av_list *)ov, &fm->base.base);
    }
    return &m->base.base;
}

const aussom_extern_method stdlib_areflect_methods[] = {
    { "instantiate", reflect_instantiate },
    { "classExists", reflect_classExists },
    { "loadedClasses", reflect_loadedClasses },
    { "loadedModules", reflect_loadedModules },
    { "isModuleLoaded", reflect_isModuleLoaded },
    { "evalStr", reflect_evalStr },
    { "evalFile", reflect_evalFile },
    { "includeModule", reflect_includeModule },
    { "invoke", reflect_invoke },
    { "getClassDef", reflect_getClassDef },
    { "parseDiagnostics", reflect_parseDiagnostics },
    { "clearParseDiagnostics", reflect_clearParseDiagnostics },
    { NULL, NULL }
};

const aussom_extern_method stdlib_aclass_methods[] = {
    { "getName", rclass_getName },
    { "isStatic", rclass_isStatic },
    { "isExtern", rclass_isExtern },
    { "getExternClassName", rclass_getExternClassName },
    { "getMembers", rclass_getMembers },
    { "getMethods", rclass_getMethods },
    { NULL, NULL }
};
