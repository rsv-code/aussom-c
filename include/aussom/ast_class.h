/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AST_CLASS_H
#define AUSSOM_AST_CLASS_H

#include <stdbool.h>

#include "aussom/ast_funct_def.h"
#include "aussom/ast_node.h"
#include "aussom/extern_registry.h"

/* One member declaration: a name plus its initializer AST. The class
 * owns both the name buffer and the value node. Insertion order is
 * preserved so the toString output matches Java. */
typedef struct ast_class_member {
    char *name;
    ast_node *value;
} ast_class_member;

typedef struct ast_class_member_array {
    ast_class_member *items;
    size_t count;
    size_t capacity;
} ast_class_member_array;

/* Array of names of classes this class extends. Each entry is heap-
 * duplicated and owned. */
typedef struct ast_class_string_array {
    char **items;
    size_t count;
    size_t capacity;
} ast_class_string_array;

/* astClass's function and member tables (functList, dispatchMap,
 * wildcardOverloads, variadicOverloads, declaredNames,
 * nullAmbiguousKeys, membDefs, allExtendedClasses). Defined in
 * ast_class.c. Built while the class is parsed; at the class's first
 * instantiation a merged copy that adds the inherited entries replaces
 * it (astClass.mergeInherited). A published table is never changed
 * again, so any thread may read it without a lock. */
typedef struct ast_class_tables ast_class_tables;

/* Mirrors com.aussom.ast.astClass.
 *
 * setName side effect: matches Java setName, which auto-adds
 * "object" to extendedClasses for any class except "object"
 * itself. Call ast_class_set_name to invoke that side effect.
 * ast_class_set_extended_classes then replaces the list, as Java's
 * setExtendedClasses does, so a class that names its parents does not
 * extend "object" directly.
 */
typedef struct ast_class {
    ast_node base;
    bool is_static;
    bool is_extern;
    char *extern_class_name;
    /* The engine's class the reference resolved to when it was parsed,
     * or NULL for a class that is not extern. Ports astClass.externClass. */
    const extern_class *bound_extern;
    ast_class_string_array extended_classes;
    /* membList: the class's own members, in declaration order. */
    ast_class_member_array members;
    /* The class's own functions, in declaration order, as parsed. */
    ast_node_array functions;
    /* The current tables. Read with ast_class_tables_of. */
    ast_class_tables *tables;
    /* astClass.initRan: set once the class has been initialized by its
     * first instantiation, or that of a class extending it. Only ever
     * goes from false to true. */
    volatile bool init_ran;
} ast_class;

ast_class *ast_class_new(void);
ast_class *ast_class_new_named(const char *name);

/* Setting the class name auto-adds "object" to extended_classes for
 * any name other than "object" itself (matches the Java override). */
void ast_class_set_name(ast_class *n, const char *name);

/* Adds a member (astClass.addMember). The class references `value`
 * via GC. */
void ast_class_add_member(ast_class *n, const char *member_name, ast_node *value);

/* astClass.addFunction: adds `funct` and registers it for calls. When
 * it collides with an overload already declared, nothing is added, the
 * earlier overload is returned and *dup_sig is set to the signature
 * Java's duplicate-overload error shows (without parentheses; "" for
 * zero slots). Returns NULL on success. */
ast_funct_def *ast_class_add_function(ast_class *n, ast_funct_def *funct,
                                      char **dup_sig);

/* astClass.setExtendedClasses: replaces the extends list. */
void ast_class_set_extended_classes(ast_class *n, char *const *names, size_t count);

/* The class's current tables, safe to read from any thread. */
ast_class_tables *ast_class_tables_of(const ast_class *n);

/* The functList of a table. A table is never changed once published,
 * so this is the class's functList as it was when the table was read. */
ast_funct_def **ast_class_tables_functions(const ast_class_tables *t, size_t *count);

/* astClass.containsFunction / getFunct: the overload registered for the
 * exact signature `sig` ("" for zero arguments). */
bool ast_class_contains_function(const ast_class *n, const char *name, const char *sig);
ast_funct_def *ast_class_get_funct(const ast_class *n, const char *name, const char *sig);

/* astClass.getFunctionsByName: the overloads named `name`, in functList
 * order. Returns a GC array and sets *count. */
ast_funct_def **ast_class_get_functions_by_name(const ast_class *n, const char *name,
                                               size_t *count);

/* astClass.hasAnyFunctionByName. */
bool ast_class_has_any_function_by_name(const ast_class *n, const char *name);

/* astClass.getAllFunctions: functList. */
ast_funct_def **ast_class_get_all_functions(const ast_class *n, size_t *count);

/* astClass.isClosureFunction: true if any overload of `name` is a
 * closure definition. Used by ast_callback_eval to decide whether
 * ::name should capture the current locals. */
bool ast_class_is_closure_function(const ast_class *n, const char *name);

/* astClass.containsMember / getMember / getMembers: membDefs. */
bool ast_class_contains_member(const ast_class *n, const char *name);
ast_node *ast_class_get_member(const ast_class *n, const char *name);
size_t ast_class_get_members(const ast_class *n, const char ***names, ast_node ***decls);

/* astClass.instanceOf: the class's own name, a name in
 * allExtendedClasses, or "null" for the cnull class. */
bool ast_class_instance_of(const ast_class *n, const char *name);

void ast_class_set_static(ast_class *n, bool is_static);
bool ast_class_get_static(const ast_class *n);

void ast_class_set_extern(ast_class *n, bool is_extern);
bool ast_class_get_extern(const ast_class *n);

/* Sets the fully qualified extern (host-language) class name. The
 * input is copied. */
void ast_class_set_extern_class_name(ast_class *n, const char *name);
const char *ast_class_get_extern_class_name(const ast_class *n);

/* The bound extern class, set once at parse time. */
void ast_class_set_bound_extern(ast_class *n, const extern_class *ec);
const extern_class *ast_class_get_bound_extern(const ast_class *n);

#endif /* AUSSOM_AST_CLASS_H */
