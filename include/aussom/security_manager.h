/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The security manager: com.aussom.SecurityManagerInt and its
 * implementations SecurityManagerImpl, DefaultSecurityManagerImpl and
 * TestSecurityManagerImpl.
 *
 * Building blocks:
 *
 * - security_manager_int is the interface: one function pointer per
 *   SecurityManagerInt method.
 * - security_manager is SecurityManagerImpl: a pointer to the method
 *   table plus `props`, the property map. Every property value is an
 *   Aussom value: bool, int, double, string, list or map.
 * - security_manager_impl_methods is SecurityManagerImpl's own method
 *   table, and security_manager_impl_* are its methods, public so a
 *   subclass can call them the way Java calls super.
 *
 * An engine is built with one manager (engine_new_with_security) and
 * holds that object, not a copy. Every gate reads it through
 * engine_get_security_manager, and the script's `secman` class forwards
 * to it.
 *
 * A host customizes policy the way a Java host subclasses
 * SecurityManagerImpl:
 *
 * - To change property values, build a manager with
 *   security_manager_impl_new and write to its props with
 *   av_strmap_put, as a Java subclass writes this.props.put.
 * - To change how a decision is made, embed security_manager as the
 *   first member of the host's own struct, initialize it with
 *   security_manager_impl_init, and point `methods` at a table whose
 *   entries are the host's overrides or the security_manager_impl_*
 *   functions it keeps. The typed reads all read through get_property,
 *   so overriding that one entry changes every decision.
 *
 * Managers are GC allocated. A host keeps one somewhere the collector
 * scans (a local variable, or GC memory) until it hands it to an
 * engine; after that the engine holds it.
 *
 * getEngineClassLoader is not carried: C has no class loader. Extern
 * classes are found through the engine's package registry
 * (engine_add_extern_package).
 */

#ifndef AUSSOM_SECURITY_MANAGER_H
#define AUSSOM_SECURITY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "aussom/av_type.h"

struct av_strmap;
struct av_list;
struct av_map;
struct environment;

typedef struct security_manager security_manager;

/* com.aussom.SecurityManagerInt. */
typedef struct security_manager_int {
    /* Java get property value: the stored value, or NULL when there is
     * none. */
    av_type *(*get_property)(security_manager *sm, const char *prop_name);

    /* Typed reads. The forms with a default answer it when the property
     * is missing or of another type; nothing is ever converted. */
    bool (*get_property_boolean)(security_manager *sm, const char *prop_name,
                                 bool default_value);
    int64_t (*get_property_int)(security_manager *sm, const char *prop_name,
                                int default_value);
    double (*get_property_double)(security_manager *sm, const char *prop_name,
                                  double default_value);
    const char *(*get_property_string)(security_manager *sm,
                                       const char *prop_name,
                                       const char *default_value);
    /* A copy, or NULL when the property is missing or not a list. */
    struct av_list *(*get_property_list)(security_manager *sm,
                                         const char *prop_name);
    /* A copy, or NULL when the property is missing or not a map. */
    struct av_map *(*get_property_map)(security_manager *sm,
                                       const char *prop_name);

    /* Aussom get or set property. Each may answer with a not-permitted
     * exception. */
    av_type *(*get_prop)(security_manager *sm, struct environment *env,
                         struct av_list *args);
    av_type *(*key_set)(security_manager *sm, struct environment *env,
                        struct av_list *args);
    av_type *(*get_map)(security_manager *sm, struct environment *env,
                        struct av_list *args);
    av_type *(*set_prop)(security_manager *sm, struct environment *env,
                         struct av_list *args);
    av_type *(*set_map)(security_manager *sm, struct environment *env,
                        struct av_list *args);
} security_manager_int;

/* com.aussom.SecurityManagerImpl. */
struct security_manager {
    const security_manager_int *methods;
    /* The properties, name -> value. */
    struct av_strmap *props;
};

/* --- SecurityManagerImpl ------------------------------------------ */

extern const security_manager_int security_manager_impl_methods;

/* new SecurityManagerImpl(): a manager holding the standard
 * properties. */
security_manager *security_manager_impl_new(void);

/* The SecurityManagerImpl constructor, for a manager embedded in a
 * host's own struct: sets the method table and a fresh props map
 * holding the standard properties. */
void security_manager_impl_init(security_manager *sm);

av_type *security_manager_impl_get_property(security_manager *sm,
                                            const char *prop_name);
bool security_manager_impl_get_property_boolean(security_manager *sm,
                                                const char *prop_name,
                                                bool default_value);
int64_t security_manager_impl_get_property_int(security_manager *sm,
                                               const char *prop_name,
                                               int default_value);
double security_manager_impl_get_property_double(security_manager *sm,
                                                 const char *prop_name,
                                                 double default_value);
const char *security_manager_impl_get_property_string(security_manager *sm,
                                                      const char *prop_name,
                                                      const char *default_value);
struct av_list *security_manager_impl_get_property_list(security_manager *sm,
                                                        const char *prop_name);
struct av_map *security_manager_impl_get_property_map(security_manager *sm,
                                                      const char *prop_name);
av_type *security_manager_impl_get_prop(security_manager *sm,
                                        struct environment *env,
                                        struct av_list *args);
av_type *security_manager_impl_key_set(security_manager *sm,
                                       struct environment *env,
                                       struct av_list *args);
av_type *security_manager_impl_get_map(security_manager *sm,
                                       struct environment *env,
                                       struct av_list *args);
av_type *security_manager_impl_set_prop(security_manager *sm,
                                        struct environment *env,
                                        struct av_list *args);
av_type *security_manager_impl_set_map(security_manager *sm,
                                       struct environment *env,
                                       struct av_list *args);

/* --- calls through the interface ------------------------------------
 *
 * What Java writes as secman.getPropertyBoolean(...): each calls the
 * manager's own method, so a subclass override is honored. */

av_type *security_manager_get_property(security_manager *sm,
                                       const char *prop_name);
bool security_manager_get_property_boolean(security_manager *sm,
                                           const char *prop_name,
                                           bool default_value);
int64_t security_manager_get_property_int(security_manager *sm,
                                          const char *prop_name,
                                          int default_value);
double security_manager_get_property_double(security_manager *sm,
                                            const char *prop_name,
                                            double default_value);
const char *security_manager_get_property_string(security_manager *sm,
                                                 const char *prop_name,
                                                 const char *default_value);
struct av_list *security_manager_get_property_list(security_manager *sm,
                                                   const char *prop_name);
struct av_map *security_manager_get_property_map(security_manager *sm,
                                                 const char *prop_name);

/* --- DefaultSecurityManagerImpl ------------------------------------
 *
 * The manager the CLI runs scripts under: the standard properties with
 * aussomdoc.file.getJson and aussomdoc.class.getJson on. */
security_manager *default_security_manager_impl_new(void);

/* --- TestSecurityManagerImpl ---------------------------------------
 *
 * The manager tests run under: the standard properties with the
 * aussomdoc reads, mocking, script mode and debugger attach on.
 * test.aussom.runner stays off until set_allow_aussom_test_runner. */
security_manager *test_security_manager_impl_new(void);
void test_security_manager_impl_set_allow_aussom_test_runner(
    security_manager *sm, bool allow_aussom_test_runner);

#endif /* AUSSOM_SECURITY_MANAGER_H */
