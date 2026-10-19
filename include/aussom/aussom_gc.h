/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Boehm GC integration shim.
 *
 * Wraps the libgc API behind project-local macros so the rest of the
 * codebase doesn't need to include <gc.h> directly. This is the only
 * file that knows we're using a conservative collector; if we swap
 * allocators later, only this header changes.
 *
 * Usage rules:
 *
 *   - Use AUSSOM_ALLOC(n) for blocks that contain pointers (every
 *     struct that holds an ast_node*, char*, function pointer, etc.).
 *     The collector scans these.
 *
 *   - Use AUSSOM_ALLOC_ATOMIC(n) for blocks that are pure bytes (C
 *     strings, raw buffers). The collector does not scan these,
 *     which is both faster and avoids spurious retention from
 *     bit-patterns that happen to look like pointers.
 *
 *   - Use AUSSOM_REALLOC for resizing pointer-bearing arrays.
 *
 *   - Use AUSSOM_STRDUP to copy a C string into atomic GC memory.
 *
 *   - Never call free(). The collector reclaims unreachable memory.
 *
 *   - Memory returned by libc (open_memstream, fopen, etc.) is not
 *     GC-managed and must still be released with libc free / fclose
 *     in the same scope that produced it.
 *
 * Call aussom_gc_init() once at program start (typically the first
 * line of main) before allocating any AST nodes.
 *
 * Threads:
 *
 * The collector runs in thread mode (GC_THREADS, set by the build).
 * Aussom itself exposes no threading to user code, but an embedder
 * may run several interpreter threads, and runtime value objects
 * (AussomList / AussomMap) can be shared between them, so the heap
 * must be thread-safe. There are two ways a thread becomes known to
 * the collector, and a given thread must use exactly ONE of them:
 *
 *   1. PREFERRED -- create it with pthread_create. With GC_THREADS,
 *      <gc.h> redirects pthread_create to the GC's wrapper, which
 *      registers and unregisters the thread automatically. Code that
 *      spawns threads must include this header so the redirect is in
 *      scope. Do NOT also call the register helpers below for such a
 *      thread (double registration aborts the process).
 *
 *   2. For a thread the collector did not create -- e.g. a host
 *      application thread that calls into Aussom -- bracket the
 *      Aussom work with aussom_gc_register_thread() /
 *      aussom_gc_unregister_thread().
 */

#ifndef AUSSOM_AUSSOM_GC_H
#define AUSSOM_AUSSOM_GC_H

#include <stddef.h>

#if !defined(GC_THREADS)
#error "GC_THREADS must be defined by the build (-DGC_THREADS) so every \
translation unit shares the same thread-aware <gc.h> view."
#endif

#include <gc.h>
#include <stdint.h>

/* Bytes this thread has been handed since it started.
 *
 * A flow counter, not a live-set size: it only ever grows, and says
 * nothing about what is still reachable. Engine accounting reads it as
 * a baseline on entry and a difference on exit, which is how a
 * per-engine allocation total is attributed to the thread that ran the
 * program. See engine_enter_interpreter_thread.
 *
 * Counted here rather than by asking the collector because Boehm only
 * reports process-wide totals; there is no per-thread counter to ask
 * for. Keeping the count at this seam is also what makes it survive
 * the allocator being replaced: everything routes through these three
 * entry points, so a different memory manager -- or an embedder's own
 * allocator -- feeds the same counter without any caller changing.
 *
 * Thread-local, so no synchronization and no contention. The cost on
 * the allocation path is one add. */
extern _Thread_local uint64_t aussom_thread_alloc_bytes;

/* The allocation entry points. Inline wrappers rather than macros so
 * the size argument is evaluated exactly once even though it is used
 * twice. */
static inline void *aussom_alloc(size_t n)
{
    aussom_thread_alloc_bytes += n;
    return GC_MALLOC(n);
}

static inline void *aussom_alloc_atomic(size_t n)
{
    aussom_thread_alloc_bytes += n;
    return GC_MALLOC_ATOMIC(n);
}

/* Counts the full new size rather than the growth. The old size is not
 * passed in, and a reallocation that has to move really does hand out
 * n fresh bytes; a grow-in-place is the over-count. Erring high on a
 * resize is the right direction for a counter a host uses to notice a
 * program allocating too much. */
static inline void *aussom_realloc(void *p, size_t n)
{
    aussom_thread_alloc_bytes += n;
    return GC_REALLOC(p, n);
}

#define AUSSOM_ALLOC(n)         aussom_alloc((n))
#define AUSSOM_ALLOC_ATOMIC(n)  aussom_alloc_atomic((n))
#define AUSSOM_REALLOC(p, n)    aussom_realloc((p), (n))

/* Returns a freshly allocated GC-managed copy of `s`. Returns a
 * pointer to an empty string when `s` is NULL so call sites don't
 * have to NULL-check. Never returns NULL except under allocation
 * failure (in which case libgc has already called the configured
 * out-of-memory handler). */
char *aussom_strdup(const char *s);

#define AUSSOM_STRDUP(s)        aussom_strdup((s))

/* Initialize the collector. Must be called once from the primordial
 * (main) thread before any AUSSOM_ALLOC and before any other thread
 * is created. Installs the abort-on-OOM handler and enables the
 * manual thread-registration path (see register helpers below). */
void aussom_gc_init(void);

/* Register the calling thread with the collector. Use ONLY for a
 * thread the collector did not create (model 2 in the threading note
 * above); never for a thread spawned via the redirected
 * pthread_create. Captures the caller's stack base, so call it from
 * the thread's own entry, not on its behalf from elsewhere. */
void aussom_gc_register_thread(void);

/* Unregister the calling thread. Pair with aussom_gc_register_thread
 * before the thread exits. */
void aussom_gc_unregister_thread(void);

#endif /* AUSSOM_AUSSOM_GC_H */
