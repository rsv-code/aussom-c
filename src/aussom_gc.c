/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/aussom_gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Definition for the counter declared in the header. Zero-initialized
 * per thread by the C runtime. */
_Thread_local uint64_t aussom_thread_alloc_bytes = 0;

/* Out-of-memory handler installed by aussom_gc_init. Aborts the
 * process so callers never have to handle NULL returns from
 * AUSSOM_ALLOC. The libgc default returns NULL on OOM, which would
 * propagate through the codebase and require defensive checks
 * everywhere. */
static void *aussom_oom_handler(size_t bytes)
{
    fprintf(stderr, "aussom: out of memory (requested %zu bytes)\n", bytes);
    abort();
    /* unreachable */
    return NULL;
}

/* Heap profile selection. Android and iOS are detected; a Raspberry Pi
 * cannot be, because it is ordinary aarch64 Linux and no predefine
 * distinguishes it from an aarch64 server. Treat it as mobile: such a
 * build defines AUSSOM_GC_MOBILE_PROFILE itself. */
#ifndef AUSSOM_GC_MOBILE_PROFILE
#  if defined(__ANDROID__)
#    define AUSSOM_GC_MOBILE_PROFILE 1
#  elif defined(__APPLE__)
#    include <TargetConditionals.h>
#    if TARGET_OS_IPHONE
#      define AUSSOM_GC_MOBILE_PROFILE 1
#    else
#      define AUSSOM_GC_MOBILE_PROFILE 0
#    endif
#  else
#    define AUSSOM_GC_MOBILE_PROFILE 0
#  endif
#endif

#if defined(__APPLE__)

/*
 * Decides which of the dynamic loader's images the collector scans.
 *
 * iOS loads system frameworks lazily for the whole life of a process --
 * taking a screenshot makes UIKit soft link another one -- and the
 * collector registers each arrival's data sections as root ranges. The
 * table those go in is fixed, and when it fills the collector aborts the
 * process with "Too many root sets", inside whatever happened to be
 * running.
 *
 * Nothing this program owns is ever reachable only from a system
 * framework's data, so those images are cost without benefit: every
 * collection walks them, and eventually one of them ends the process.
 *
 * Declining them here rather than turning image registration off
 * altogether is deliberate. The collector still registers this program's
 * own image, through its own code, which knows how to find a section and
 * how to apply the load address. Doing that by hand means reimplementing
 * it, and getting the address wrong produces a root range over unrelated
 * memory -- which is not a crash at the point of the mistake but a
 * pointer built out of whatever bytes were there.
 *
 * The test errs toward keeping an image: only the two directories every
 * Apple system library lives in are declined, and anything unrecognized
 * is scanned. Missing a root is silent corruption; scanning one image too
 * many is only work.
 */
static int GC_CALLBACK aussom_gc_scan_image(const char *dlpi_name,
                                            void *section_start,
                                            size_t section_size)
{
    (void)section_start;
    (void)section_size;

    if (dlpi_name == NULL) return 1;
    if (strncmp(dlpi_name, "/System/", 8) == 0) return 0;
    if (strncmp(dlpi_name, "/usr/lib/", 9) == 0) return 0;
    return 1;
}

#endif /* __APPLE__ */

void aussom_gc_init(void)
{
#if defined(__APPLE__)
    /* Before GC_INIT, which registers everything already loaded. */
    GC_register_has_static_roots_callback(aussom_gc_scan_image);
#endif

    /* GC_INIT is required on platforms where the collector cannot
     * reliably locate the bottom of the main thread's stack on its
     * own, and it must run on the primordial thread. */
    GC_INIT();
    GC_set_oom_fn(aussom_oom_handler);

    /* Heap growth policy, chosen per platform.
     *
     * Desktop pre-grows the heap so allocation-heavy workloads (deep
     * recursion, tight member-access loops) don't thrash through the
     * early collection cycles that a tiny starting heap forces. The
     * tree walker allocates a call frame (locals map + environment +
     * stack frame) per call and a boxed value per arithmetic result, so
     * the allocation rate is high; the default starting heap collects
     * far too eagerly for that, which was the dominant cost in the
     * deep-call and member-access benchmarks. The reservation is lazily
     * committed (a trivial script's RSS rises by well under 1 MiB) and
     * the heap still shrinks back, so this is nearly free for small
     * programs and a large win (~40%) for heavy ones.
     *
     * Mobile does not pre-grow. On iOS and Android the process is
     * killed on footprint, so a resident reservation costs more than
     * the collections it saves; a small live heap collects in single
     * digit milliseconds, which makes the trade affordable.
     *
     * Either way the value is overridable upward via the collector's
     * standard GC_INITIAL_HEAP_SIZE environment variable. */
#if !AUSSOM_GC_MOBILE_PROFILE
    GC_expand_hp((size_t)64 * 1024 * 1024);
#endif

    /* Enable the explicit thread-registration API so host threads
     * that the collector did not create can register themselves via
     * aussom_gc_register_thread. Must be called before any such
     * thread starts; coexists with the redirected pthread_create
     * (auto-registered) path. */
    GC_allow_register_threads();
}

void aussom_gc_register_thread(void)
{
    struct GC_stack_base sb;
    GC_get_stack_base(&sb);
    GC_register_my_thread(&sb);
}

void aussom_gc_unregister_thread(void)
{
    GC_unregister_my_thread();
}

char *aussom_strdup(const char *s)
{
    const char *src = (s != NULL) ? s : "";
    size_t len = strlen(src) + 1;
    char *out = AUSSOM_ALLOC_ATOMIC(len);
    memcpy(out, src, len);
    return out;
}
