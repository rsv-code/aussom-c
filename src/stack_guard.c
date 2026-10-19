/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * See aussom/stack_guard.h.
 */

#if defined(__linux__) || defined(__ANDROID__)
/* pthread_getattr_np is a GNU extension, hidden by the build's
 * _POSIX_C_SOURCE unless asked for. */
#  define _GNU_SOURCE
#endif

#include "aussom/stack_guard.h"

#if defined(_WIN32)
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0602   /* GetCurrentThreadStackLimits: Windows 8 */
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#endif

_Thread_local stack_guard_state stack_guard_tls;

/* The calling thread's stack as [*low, *high). False when the OS will
 * not say. */
static bool stack_range(uintptr_t *low, uintptr_t *high)
{
#if defined(_WIN32)
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    *low = (uintptr_t)lo;
    *high = (uintptr_t)hi;
#elif defined(__APPLE__)
    /* Apple reports the high end (the stack's base) and the size. */
    pthread_t self = pthread_self();
    uintptr_t base = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    if (base == 0 || size == 0 || size > base) return false;
    *low = base - size;
    *high = base;
#else
    /* glibc and bionic both answer for the main thread too, reading
     * the stack rlimit and the process mappings. */
    pthread_attr_t attr;
    void *addr = NULL;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
    int rc = pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || size == 0) return false;
    *low = (uintptr_t)addr;
    *high = (uintptr_t)addr + size;
#endif
    return *low != 0 && *high > *low;
}

void stack_guard_init_thread(stack_guard_state *s)
{
    uintptr_t low = 0, high = 0;
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    /* Off when the bounds are unknown, when this frame is not inside
     * them, or when the thread is already inside the reserve (a stack
     * smaller than the reserve itself): reporting overflow on every
     * check would stop all work. */
    if (!stack_range(&low, &high) || here >= high
            || here <= low + STACK_GUARD_RESERVE) {
        s->limit = 0;
        s->high = UINTPTR_MAX;
    } else {
        s->limit = low + STACK_GUARD_RESERVE;
        s->high = high;
    }
    s->ready = true;
}
