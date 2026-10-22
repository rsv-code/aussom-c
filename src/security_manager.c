/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * See aussom/security_manager.h. Ports com.aussom.SecurityManagerImpl,
 * DefaultSecurityManagerImpl and TestSecurityManagerImpl.
 */

#include <stdio.h>
#include <string.h>

#include "aussom/security_manager.h"

#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/av_strmap.h"
#include "aussom/engine.h"
#include "aussom/environment.h"
#include "aussom/stack_guard.h"

static void props_put_bool(av_strmap *p, const char *name, bool v)
{
    av_strmap_put(p, name, &av_bool_new(v)->base.base);
}

static void props_put_int(av_strmap *p, const char *name, int64_t v)
{
    av_strmap_put(p, name, &av_int_new(v)->base.base);
}

/* --- SecurityManagerImpl -------------------------------------------- */

/* The SecurityManagerImpl constructor's property list, in its order. */
static void seed_standard_props(av_strmap *p)
{
    /* Security manager itself. instantiate: can new instances be
     * created? Checked against the running engine's manager by the
     * Aussom SecurityManager constructor. */
    props_put_bool(p, "securitymanager.instantiate", true);
    props_put_bool(p, "securitymanager.property.get", true);    /* getProp */
    props_put_bool(p, "securitymanager.property.list", true);   /* keySet/getMap */
    props_put_bool(p, "securitymanager.property.set", false);   /* setProp */

    /* System information view. See stdlib/sys_methods.c. */
    props_put_bool(p, "os.info.view", false);
    props_put_bool(p, "java.info.view", false);
    props_put_bool(p, "java.home.view", false);
    props_put_bool(p, "java.classpath.view", false);
    props_put_bool(p, "aussom.info.view", false);
    props_put_bool(p, "aussom.path.view", false);
    props_put_bool(p, "current.path.view", false);
    props_put_bool(p, "home.path.view", false);
    props_put_bool(p, "user.name.view", false);

    /* Reflection actions. See stdlib/reflect_methods.c. */
    props_put_bool(p, "reflect.eval.string", false);
    props_put_bool(p, "reflect.eval.file", false);
    props_put_bool(p, "reflect.include.module", false);
    /* Discards collected parse diagnostics, so it is gated with the
     * other reflect actions that change engine state. Reading them back
     * (reflect.parseDiagnostics) is not gated, matching the other
     * read-only reflect accessors. */
    props_put_bool(p, "reflect.clear.diagnostics", false);

    /* Aussomdoc actions. See stdlib/lang_methods.c. */
    props_put_bool(p, "aussomdoc.file.getJson", false);
    props_put_bool(p, "aussomdoc.class.getJson", false);
    /* Keep doc comment text on the AST. Unlike the two above this is
     * parse behavior rather than an action gate. False drops the text
     * as each doc comment is parsed, which blanks doc text for every
     * consumer: -d output, reflect, and any tooling built on them. True
     * is the default because that loss is silent. */
    props_put_bool(p, "aussomdoc.retain", true);

    /* Unit testing actions. */
    props_put_bool(p, "test.aussom.runner", false);
    props_put_bool(p, "test.mock.inject", false);
    props_put_bool(p, "test.mock.spy", false);

    /* Script mode. See engine_set_script_mode. */
    props_put_bool(p, "aussom.script.mode.enable", false);

    /* Debugger attach. */
    props_put_bool(p, "aussom.debugger.enable", false);

    /* Extern class binding. See engine_is_extern_class_allowed. With
     * enforce false the list is ignored. With it true every 'extern
     * class' declaration is checked, including the standard library's,
     * so the list is a complete statement of what this engine may bind.
     * The default list is what the shipped modules need. An entry is an
     * exact class name or a 'pkg.*' prefix. */
    props_put_bool(p, "aussom.extern.allowlist.enforce", false);
    av_list *allowed = av_list_new();
    av_list_add(allowed, &av_string_new("com.aussom.stdlib.*")->base.base);
    av_list_add(allowed, &av_string_new("com.aussom.types.*")->base.base);
    av_strmap_put(p, "aussom.extern.allowed", &allowed->base.base);

    /* Runtime settings read by engine_refresh_limits. */
    props_put_int(p, ENGINE_LIMIT_CALL_DEPTH_PROP, ENGINE_DEFAULT_CALL_DEPTH);
    /* 0 means no budget, so regex behavior is unchanged unless a host
     * asks for one. */
    props_put_int(p, ENGINE_LIMIT_REGEX_STEPS_PROP, 0);
    /* How long a sleeping program may run before the interpreter looks
     * at the engine's control state again. 0 turns slicing off. */
    props_put_int(p, ENGINE_LIMIT_SLEEP_SLICE_PROP, ENGINE_DEFAULT_SLEEP_SLICE_MS);
    /* Largest source file the engine will parse, 0 for no limit. */
    props_put_int(p, ENGINE_LIMIT_SOURCE_BYTES_PROP, 0);

    /* Whether an include may be reached through a symbolic link. A host
     * whose include root is writable by untrusted tenants sets it false.
     * See include_resolve.c. */
    props_put_bool(p, "aussom.include.symlink.follow", true);
}

void security_manager_impl_init(security_manager *sm)
{
    sm->methods = &security_manager_impl_methods;
    sm->props = av_strmap_new();
    seed_standard_props(sm->props);
}

security_manager *security_manager_impl_new(void)
{
    security_manager *sm = AUSSOM_ALLOC(sizeof(*sm));
    security_manager_impl_init(sm);
    return sm;
}

av_type *security_manager_impl_get_property(security_manager *sm,
                                            const char *prop_name)
{
    return av_strmap_get(sm->props, prop_name);
}

/* Typed property reads. These are what gates use. Each reads through
 * get_property rather than the props map, so a subclass that overrides
 * get_property to compute a value is honored here too. No value is
 * ever converted: a property of the wrong type is not a match. */

bool security_manager_impl_get_property_boolean(security_manager *sm,
                                                const char *prop_name,
                                                bool default_value)
{
    av_type *o = sm->methods->get_property(sm, prop_name);
    if (o != NULL && o->type == C_TYPE_BOOL) return av_bool_get((av_bool *)o);
    return default_value;
}

int64_t security_manager_impl_get_property_int(security_manager *sm,
                                               const char *prop_name,
                                               int default_value)
{
    av_type *o = sm->methods->get_property(sm, prop_name);
    if (o != NULL && o->type == C_TYPE_INT) return av_int_get((av_int *)o);
    return default_value;
}

double security_manager_impl_get_property_double(security_manager *sm,
                                                 const char *prop_name,
                                                 double default_value)
{
    av_type *o = sm->methods->get_property(sm, prop_name);
    if (o != NULL && o->type == C_TYPE_DOUBLE) return av_double_get((av_double *)o);
    return default_value;
}

const char *security_manager_impl_get_property_string(security_manager *sm,
                                                      const char *prop_name,
                                                      const char *default_value)
{
    av_type *o = sm->methods->get_property(sm, prop_name);
    if (o != NULL && o->type == C_TYPE_STRING) return av_string_get((av_string *)o);
    return default_value;
}

/* The list and map reads return copies, so a caller cannot reach back
 * into policy through a value it was handed. */

av_list *security_manager_impl_get_property_list(security_manager *sm,
                                                 const char *prop_name)
{
    av_type *o = sm->methods->get_property(sm, prop_name);
    if (o == NULL || o->type != C_TYPE_LIST) return NULL;
    return (av_list *)av_clone(o);
}

av_map *security_manager_impl_get_property_map(security_manager *sm,
                                               const char *prop_name)
{
    av_type *o = sm->methods->get_property(sm, prop_name);
    if (o == NULL || o->type != C_TYPE_MAP) return NULL;
    return (av_map *)av_clone(o);
}

/* A stored value in its Aussom form. Values are copied in both
 * directions, so a script cannot hold a reference and edit policy
 * after a write was permitted. Ports toAussom. */
static av_type *to_aussom(av_type *v)
{
    if (v == NULL) return &av_null_new()->base.base;
    return av_clone(v);
}

/* What may be stored as a property: a simple value, or a list or map
 * whose contents are themselves storable. Anything else is refused, so
 * an object or callback cannot be smuggled into the policy map. Ports
 * storable.
 *
 * Reads containers through snapshots, so a value that holds itself
 * recurses until the stack guard stops it (false, thread tripped)
 * instead of waiting on its own lock. */
static bool storable(av_type *v)
{
    if (stack_guard_low()) return false;
    if (v == NULL) return true;
    switch (v->type) {
    case C_TYPE_NULL:
    case C_TYPE_BOOL:
    case C_TYPE_INT:
    case C_TYPE_DOUBLE:
    case C_TYPE_STRING:
        return true;
    case C_TYPE_LIST: {
        size_t n;
        av_type **items = av_vec_snapshot(av_list_vec((av_list *)v), &n);
        for (size_t i = 0; i < n; i++) {
            if (!storable(items[i])) return false;
        }
        return true;
    }
    case C_TYPE_MAP: {
        const char **keys;
        av_type **vals;
        size_t n = av_strmap_snapshot(av_map_strmap((av_map *)v), &keys, &vals);
        for (size_t i = 0; i < n; i++) {
            if (!storable(vals[i])) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

/* The Java class name the setProp/setMap refusal names
 * (ct.getClass().getName()). */
static const char *java_type_name(const av_type *v)
{
    switch (v->type) {
    case C_TYPE_OBJECT:    return "com.aussom.types.AussomObject";
    /* A list or map that holds something unstorable is named itself. */
    case C_TYPE_LIST:      return "com.aussom.types.AussomList";
    case C_TYPE_MAP:       return "com.aussom.types.AussomMap";
    case C_TYPE_CALLBACK:  return "com.aussom.types.AussomCallback";
    case C_TYPE_EXCEPTION: return "com.aussom.types.AussomException";
    default:               return "com.aussom.types.AussomType";
    }
}

static av_type *not_storable(const char *method, const av_type *v)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
             "securitymanager.%s(): Expecting simple type, list or map but "
             "found '%s' instead.", method, java_type_name(v));
    return &av_exception_new(msg)->base.base;
}

static av_type *sm_exc(const char *msg) { return &av_exception_new(msg)->base.base; }

/* Stores a value. A null is recorded by removing the key, as Java's
 * ConcurrentHashMap requires; get_property then answers NULL either
 * way. Ports putOrRemove. */
static void put_or_remove(security_manager *sm, const char *key, av_type *v)
{
    if (v == NULL || v->type == C_TYPE_NULL) {
        av_strmap_remove(sm->props, key);
    } else {
        av_strmap_put(sm->props, key, av_clone(v));
    }
}

av_type *security_manager_impl_get_prop(security_manager *sm,
                                        environment *env, av_list *args)
{
    (void)env;
    if (sm->methods->get_property_boolean(sm, "securitymanager.property.get", false)) {
        const char *name = av_list_get_string(args, 0);
        if (name == NULL) return sm_exc("securitymanager.getProp(): argument 1 must be a string.");
        return to_aussom(av_strmap_get(sm->props, name));
    }
    return sm_exc("securitymanager.getProp(): Security exception, action "
                  "'securitymanager.property.get' not permitted.");
}

av_type *security_manager_impl_key_set(security_manager *sm,
                                       environment *env, av_list *args)
{
    (void)env;
    (void)args;
    if (sm->methods->get_property_boolean(sm, "securitymanager.property.list", false)) {
        av_list *cl = av_list_new();
        const char **keys;
        av_type **vals;
        size_t n = av_strmap_snapshot(sm->props, &keys, &vals);
        for (size_t i = 0; i < n; i++) {
            av_list_add(cl, &av_string_new(keys[i])->base.base);
        }
        return &cl->base.base;
    }
    return sm_exc("securitymanager.keySet(): Security exception, action "
                  "'securitymanager.property.list' not permitted.");
}

av_type *security_manager_impl_get_map(security_manager *sm,
                                       environment *env, av_list *args)
{
    (void)env;
    (void)args;
    if (sm->methods->get_property_boolean(sm, "securitymanager.property.list", false)) {
        av_map *cm = av_map_new();
        const char **keys;
        av_type **vals;
        size_t n = av_strmap_snapshot(sm->props, &keys, &vals);
        for (size_t i = 0; i < n; i++) {
            av_map_put(cm, keys[i], to_aussom(vals[i]));
        }
        return &cm->base.base;
    }
    return sm_exc("securitymanager.getMap(): Security exception, action "
                  "'securitymanager.property.list' not permitted.");
}

av_type *security_manager_impl_set_prop(security_manager *sm,
                                        environment *env, av_list *args)
{
    if (sm->methods->get_property_boolean(sm, "securitymanager.property.set", false)) {
        const char *key = av_list_get_string(args, 0);
        if (key == NULL) return sm_exc("securitymanager.setProp(): argument 1 must be a string.");
        av_type *ct = av_list_get(args, 1);
        if (!storable(ct)) return not_storable("setProp", ct);
        put_or_remove(sm, key, ct);
        return &environment_class_instance(env)->base;
    }
    return sm_exc("securitymanager.getProp(): Security exception, action "
                  "'securitymanager.property.set' not permitted.");
}

av_type *security_manager_impl_set_map(security_manager *sm,
                                       environment *env, av_list *args)
{
    if (sm->methods->get_property_boolean(sm, "securitymanager.property.set", false)) {
        av_map *mp = av_list_get_map(args, 0);
        if (mp == NULL) return sm_exc("securitymanager.setMap(): argument 1 must be a map.");
        /* Every value is checked before any is stored, so a refused map
         * changes nothing. */
        const char **keys;
        av_type **vals;
        size_t n = av_strmap_snapshot(av_map_strmap(mp), &keys, &vals);
        for (size_t i = 0; i < n; i++) {
            if (!storable(vals[i])) return not_storable("setMap", vals[i]);
        }
        for (size_t i = 0; i < n; i++) {
            put_or_remove(sm, keys[i], vals[i]);
        }
        return &environment_class_instance(env)->base;
    }
    return sm_exc("securitymanager.getProp(): Security exception, action "
                  "'securitymanager.property.set' not permitted.");
}

const security_manager_int security_manager_impl_methods = {
    .get_property         = security_manager_impl_get_property,
    .get_property_boolean = security_manager_impl_get_property_boolean,
    .get_property_int     = security_manager_impl_get_property_int,
    .get_property_double  = security_manager_impl_get_property_double,
    .get_property_string  = security_manager_impl_get_property_string,
    .get_property_list    = security_manager_impl_get_property_list,
    .get_property_map     = security_manager_impl_get_property_map,
    .get_prop             = security_manager_impl_get_prop,
    .key_set              = security_manager_impl_key_set,
    .get_map              = security_manager_impl_get_map,
    .set_prop             = security_manager_impl_set_prop,
    .set_map              = security_manager_impl_set_map,
};

/* --- calls through the interface ------------------------------------ */

av_type *security_manager_get_property(security_manager *sm, const char *prop_name)
{
    return sm->methods->get_property(sm, prop_name);
}

bool security_manager_get_property_boolean(security_manager *sm,
                                           const char *prop_name,
                                           bool default_value)
{
    return sm->methods->get_property_boolean(sm, prop_name, default_value);
}

int64_t security_manager_get_property_int(security_manager *sm,
                                          const char *prop_name,
                                          int default_value)
{
    return sm->methods->get_property_int(sm, prop_name, default_value);
}

double security_manager_get_property_double(security_manager *sm,
                                            const char *prop_name,
                                            double default_value)
{
    return sm->methods->get_property_double(sm, prop_name, default_value);
}

const char *security_manager_get_property_string(security_manager *sm,
                                                 const char *prop_name,
                                                 const char *default_value)
{
    return sm->methods->get_property_string(sm, prop_name, default_value);
}

av_list *security_manager_get_property_list(security_manager *sm,
                                            const char *prop_name)
{
    return sm->methods->get_property_list(sm, prop_name);
}

av_map *security_manager_get_property_map(security_manager *sm,
                                          const char *prop_name)
{
    return sm->methods->get_property_map(sm, prop_name);
}

/* --- DefaultSecurityManagerImpl -------------------------------------- */

security_manager *default_security_manager_impl_new(void)
{
    security_manager *sm = security_manager_impl_new();
    props_put_bool(sm->props, "aussomdoc.file.getJson", true);
    props_put_bool(sm->props, "aussomdoc.class.getJson", true);
    return sm;
}

/* --- TestSecurityManagerImpl ----------------------------------------- */

security_manager *test_security_manager_impl_new(void)
{
    security_manager *sm = security_manager_impl_new();
    props_put_bool(sm->props, "aussomdoc.file.getJson", true);
    props_put_bool(sm->props, "aussomdoc.class.getJson", true);
    props_put_bool(sm->props, "test.mock.inject", true);
    props_put_bool(sm->props, "test.mock.spy", true);
    props_put_bool(sm->props, "aussom.script.mode.enable", true);
    props_put_bool(sm->props, "aussom.debugger.enable", true);
    return sm;
}

void test_security_manager_impl_set_allow_aussom_test_runner(
    security_manager *sm, bool allow_aussom_test_runner)
{
    props_put_bool(sm->props, "test.aussom.runner", allow_aussom_test_runner);
}
