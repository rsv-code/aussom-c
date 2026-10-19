/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/stdlib_packages.h"

#include <stddef.h>

#include "aussom/test_runner.h"
#include "stdlib_internal.h"

/* Class names are the last segment of each reference in the stdlib
 * .aus files, the name aussom-base's class loader resolves. */

static const aussom_extern_class types_classes[] = {
    { "AussomBool", stdlib_aussom_bool_methods },
    { "AussomInt", stdlib_aussom_int_methods },
    { "AussomDouble", stdlib_aussom_double_methods },
    { "AussomString", stdlib_aussom_string_methods },
    { "AussomList", stdlib_aussom_list_methods },
    { "AussomMap", stdlib_aussom_map_methods },
    { "AussomObject", stdlib_aussom_object_methods },
    { "AussomCallback", stdlib_aussom_callback_methods },
    { "AussomNull", stdlib_aussom_null_methods },
    { "AussomException", stdlib_aussom_exception_methods },
    { NULL, NULL }
};

const aussom_extern_package stdlib_types_package = {
    "com.aussom.types", types_classes
};

static const aussom_extern_class stdlib_classes[] = {
    { "SBool", stdlib_sbool_methods },
    { "SInt", stdlib_sint_methods },
    { "SDouble", stdlib_sdouble_methods },
    { "console", stdlib_console_methods },
    { "ADate", stdlib_adate_methods },
    { "ABuffer", stdlib_abuffer_methods },
    { "ALang", stdlib_alang_methods },
    { "AJson", stdlib_ajson_methods },
    { "ASecMan", stdlib_asecman_methods },
    { "ASecurityManager", stdlib_asecurity_manager_methods },
    { "AAtomicLong", stdlib_aatomic_long_methods },
    { "AAtomicBool", stdlib_aatomic_bool_methods },
    { "AAtomicRef", stdlib_aatomic_ref_methods },
    { "ACounter", stdlib_acounter_methods },
    { "ABlockingQueue", stdlib_ablocking_queue_methods },
    { "ASemaphore", stdlib_asemaphore_methods },
    { "ALatch", stdlib_alatch_methods },
    { "ALock", stdlib_alock_methods },
    { "AMath", stdlib_amath_methods },
    { "ASys", stdlib_asys_methods },
    { "ABase64", stdlib_abase64_methods },
    { "AHex", stdlib_ahex_methods },
    { "AUuid", stdlib_auuid_methods },
    { "ARegex", stdlib_aregex_methods },
    { "ATest", stdlib_atest_methods },
    { "ATestRunner", stdlib_atest_runner_methods },
    { "AReflect", stdlib_areflect_methods },
    { "AClass", stdlib_aclass_methods },
    { NULL, NULL }
};

const aussom_extern_package stdlib_stdlib_package = {
    "com.aussom.stdlib", stdlib_classes
};
