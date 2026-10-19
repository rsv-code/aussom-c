/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `secman` and instantiable `SecurityManager` classes
 * (com.aussom.stdlib.ASecMan / ASecurityManager).
 *
 * `secman` passes each call to the engine's own security manager, so it
 * reports and changes what the running program is actually allowed to
 * do. Its property.set defaults to false.
 *
 * `new SecurityManager()` is something different. Like ASecurityManager,
 * which extends SecurityManagerImpl, it is a security manager of its own
 * (security_manager.h), starting from the standard properties. Writing
 * to it does not change what the running program may do. Constructing
 * one is gated on securitymanager.instantiate as read from the engine
 * that is running.
 */

#include <stdint.h>
#include <string.h>

#include "aussom/ast_class.h"
#include "aussom/aussom_gc.h"
#include "aussom/av.h"
#include "aussom/av_object.h"
#include "aussom/av_strmap.h"
#include "aussom/engine.h"
#include "aussom/extern_registry.h"
#include "aussom/environment.h"
#include "aussom/security_manager.h"
#include "stdlib_internal.h"

static av_type *sm_exc(const char *msg) { return &av_exception_new(msg)->base.base; }

/* --- secman (ASecMan) --------------------------------------------- */

static security_manager *engine_secman(environment *env)
{
    return engine_get_security_manager(environment_engine(env));
}

static av_type *asm_getProp(av_type *self, environment *env, av_list *args)
{
    (void)self;
    security_manager *sm = engine_secman(env);
    return sm->methods->get_prop(sm, env, args);
}

static av_type *asm_keySet(av_type *self, environment *env, av_list *args)
{
    (void)self;
    security_manager *sm = engine_secman(env);
    return sm->methods->key_set(sm, env, args);
}

static av_type *asm_getMap(av_type *self, environment *env, av_list *args)
{
    (void)self;
    security_manager *sm = engine_secman(env);
    return sm->methods->get_map(sm, env, args);
}

static av_type *asm_setProp(av_type *self, environment *env, av_list *args)
{
    (void)self;
    security_manager *sm = engine_secman(env);
    return sm->methods->set_prop(sm, env, args);
}

/* ASecMan has no setMap, although lang.aus declares one, so in Java
 * secman.setMap() fails as a missing extern method. Left out here for
 * the same result. */
const aussom_extern_method stdlib_asecman_methods[] = {
    { "getProp", asm_getProp },
    { "keySet", asm_keySet },
    { "getMap", asm_getMap },
    { "setProp", asm_setProp },
    { NULL, NULL }
};

/* --- SecurityManager (ASecurityManager) ---------------------------- */

/* A SecurityManager instance stores one of these as its extern_object.
 * The tag is how instance_secman tells its own state from anything
 * else: a script-written binding or a subclass leaves extern_object
 * pointing at an object, possibly another extern class's state, and
 * reading that as a security_manager would call through a function
 * table taken from the wrong struct. First field, so the
 * read is always in bounds. security_manager itself is public and
 * host-built, so the tag lives in this wrapper rather than in it. */
#define SECMAN_MAGIC 0x534d4752u   /* "SMGR" */
typedef struct secman_box { uint32_t magic; security_manager *sm; } secman_box;

/* This instance's own manager. Java builds the ASecurityManager (a
 * SecurityManagerImpl with the standard properties) when the object is
 * instantiated, before the constructor runs. C attaches it on first
 * use instead: until then extern_object points at the object itself,
 * or at whatever a subclass copied from a parent. Anything without the
 * tag gets a fresh manager. */
static security_manager *instance_secman(av_type *self)
{
    av_object *o = (av_object *)self;
    void *eo = av_object_get_extern_object(o);
    if (eo != NULL && eo != (void *)o &&
        ((secman_box *)eo)->magic == SECMAN_MAGIC) {
        return ((secman_box *)eo)->sm;
    }
    secman_box *box = AUSSOM_ALLOC(sizeof(*box));
    box->magic = SECMAN_MAGIC;
    box->sm = security_manager_impl_new();
    av_object_set_extern_object(o, box);
    return box->sm;
}

/* The Aussom `SecurityManager()` constructor. Gated on
 * securitymanager.instantiate as read from the engine that is running,
 * which is the point: a check that read the new object's own copy of
 * the property could never fail. On success it opens up this object's
 * own get, list and set permissions, so a script can read and write the
 * policy value it just created. That is safe because the object is not
 * the engine's manager. Ports ASecurityManager.newSecurityManager. */
static av_type *sm_newSecurityManager(av_type *self, environment *env, av_list *args)
{
    (void)args;
    if (!security_manager_get_property_boolean(engine_secman(env),
            "securitymanager.instantiate", false)) {
        return sm_exc("SecurityManager(): Security exception, action "
                      "'securitymanager.instantiate' not permitted.");
    }
    security_manager *sm = instance_secman(self);
    av_strmap_put(sm->props, "securitymanager.property.get", &av_bool_new(true)->base.base);
    av_strmap_put(sm->props, "securitymanager.property.list", &av_bool_new(true)->base.base);
    av_strmap_put(sm->props, "securitymanager.property.set", &av_bool_new(true)->base.base);
    return &environment_class_instance(env)->base;
}

#define INSTANCE_METHOD(cname, member)                                       \
    static av_type *cname(av_type *self, environment *env, av_list *args)   \
    {                                                                        \
        security_manager *sm = instance_secman(self);                        \
        return sm->methods->member(sm, env, args);                           \
    }

INSTANCE_METHOD(sm_getProp, get_prop)
INSTANCE_METHOD(sm_keySet, key_set)
INSTANCE_METHOD(sm_getMap, get_map)
INSTANCE_METHOD(sm_setProp, set_prop)
INSTANCE_METHOD(sm_setMap, set_map)

const aussom_extern_method stdlib_asecurity_manager_methods[] = {
    { "getProp", sm_getProp },
    { "keySet", sm_keySet },
    { "getMap", sm_getMap },
    { "setProp", sm_setProp },
    { "setMap", sm_setMap },
    { "newSecurityManager", sm_newSecurityManager },
    { NULL, NULL }
};
