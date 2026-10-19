/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The concurrency primitives from concurrent.aus (com.aussom.stdlib
 * AAtomicLong / AAtomicBool / AAtomicRef / ACounter / ABlockingQueue /
 * ASemaphore / ALatch / ALock).
 *
 * Aussom itself creates no threads, but an embedder may evaluate one
 * shared engine from several threads at once, and script-level compound
 * operations on shared members are not atomic (`this.n += 1` is a
 * separate read, compute and write). These classes are the atomic
 * building blocks for that case.
 *
 * The contract every class here keeps: EACH METHOD CALL IS ATOMIC, A
 * SEQUENCE OF METHOD CALLS IS NOT.
 *
 * Implementation notes:
 *  - Each object owns a pthread mutex, and the blocking types add a
 *    condition variable. This mirrors the per-object locking the rest
 *    of the runtime already uses (av_vec / av_strmap); it stands in for
 *    java.util.concurrent, which has no C equivalent.
 *  - State lives in a GC-allocated struct hung off the av_object
 *    extern_object pointer, the same pattern Buffer and Date use. A
 *    default glibc mutex/cond owns no kernel resource, so letting the
 *    collector reclaim it is safe and no destroy pass is needed.
 *  - Timed waits use pthread_cond_timedwait against CLOCK_REALTIME,
 *    which is what the default condattr uses.
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "aussom/ast_eval.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/extern_registry.h"
#include "stdlib_internal.h"

static av_type *conc_exc(const char *msg) { return &av_exception_new(msg)->base.base; }

static int64_t arg_i(av_list *a, size_t i)
{
    if (av_list_size(a) <= i) return 0;
    av_type *v = av_list_get(a, i);
    if (v->type == C_TYPE_NULL) return 0;
    return av_numeric_int(v);
}

static bool arg_b(av_list *a, size_t i)
{
    if (av_list_size(a) <= i) return false;
    av_type *v = av_list_get(a, i);
    return v->type == C_TYPE_BOOL && av_bool_get((av_bool *)v);
}

/* Absolute CLOCK_REALTIME deadline `ms` from now, for the timed waits.
 * A deadline past what time_t holds (a 32-bit time_t on armv7 runs out
 * in 2038) becomes the latest time_t, so a huge timeout waits as long as
 * it can, as Java's timed waits do with Long.MAX_VALUE, instead of
 * wrapping into the past and returning at once. */
static void deadline_in(int64_t ms, struct timespec *ts)
{
    const time_t tmax = (time_t)((sizeof(time_t) == 8) ? INT64_MAX : INT32_MAX);
    clock_gettime(CLOCK_REALTIME, ts);
    int64_t sec = ms / 1000;
    long nsec = ts->tv_nsec + (long)((ms % 1000) * 1000000L);
    if (nsec >= 1000000000L) { sec++; nsec -= 1000000000L; }
    if (sec > (int64_t)(tmax - ts->tv_sec)) {
        ts->tv_sec = tmax;
        ts->tv_nsec = 999999999L;
        return;
    }
    ts->tv_sec += (time_t)sec;
    ts->tv_nsec = nsec;
}

/* --- shared state layout ------------------------------------------ */

/* One struct serves every class here; each uses the fields it needs.
 * Keeping it uniform means one accessor and one constructor helper. */
/* Tags a genuine conc_state so an accessor can tell it apart from a
 * plain av_object. A confused `extern class ... : com.aussom.stdlib.A*`
 * binding, or a subclass that copied its parent's extern_object (which
 * for the constructor-less Counter/Lock is the parent object itself)
 * has no real state; the tag is how we detect that without
 * reading past the object. First field so the read is always in bounds. */
#define CONC_MAGIC 0x434f4e43u   /* "CONC" */

typedef struct conc_state {
    uint32_t magic;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int64_t ival;          /* AtomicLong / Counter / Semaphore / Latch */
    bool bval;             /* AtomicBool */
    av_type *ref;          /* AtomicRef */
    av_vec *queue;         /* BlockingQueue */
    int64_t capacity;      /* BlockingQueue, 0 = unbounded */
    int lock_depth;        /* Lock: reentrancy count */
    pthread_t owner;       /* Lock: current holder when lock_depth > 0 */
    bool owned;
} conc_state;

static conc_state *state_new(av_type *self);

/* Returns the receiver's real conc_state, creating a fresh default one
 * if the receiver was never initialized (a confused binding) or
 * carries a copied parent object instead of state (a Counter / Lock
 * subclass). The magic tag distinguishes a real state
 * from any av_object; reading it is always in bounds (4 bytes at
 * offset 0 of an object that is at least that large). This makes every
 * caller memory-safe without a NULL check of its own. */
static conc_state *self_state(av_type *self)
{
    if (self != NULL && self->type == C_TYPE_OBJECT) {
        void *eo = av_object_get_extern_object((av_object *)self);
        if (eo != NULL && eo != (void *)self &&
            ((conc_state *)eo)->magic == CONC_MAGIC) {
            return (conc_state *)eo;
        }
    }
    return state_new(self);
}

/* Builds the state struct and attaches it, replacing the default
 * extern_object that class_instantiate set to the instance itself. */
static conc_state *state_new(av_type *self)
{
    conc_state *s = AUSSOM_ALLOC(sizeof(*s));
    s->magic = CONC_MAGIC;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->ival = 0;
    s->bval = false;
    s->ref = &av_null_new()->base.base;
    s->queue = NULL;
    s->capacity = 0;
    s->lock_depth = 0;
    s->owned = false;
    av_object_set_extern_object((av_object *)self, s);
    return s;
}

/* --- AtomicLong --------------------------------------------------- */

static av_type *al_new(av_type *self, environment *env, av_list *args)
{
    (void)env;
    state_new(self)->ival = arg_i(args, 0);
    return self;
}

static av_type *al_get(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    int64_t v = s->ival;
    pthread_mutex_unlock(&s->lock);
    return &av_int_new(v)->base.base;
}

static av_type *al_set(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    s->ival = arg_i(args, 0);
    pthread_mutex_unlock(&s->lock);
    return self;
}

/* Adds as Java's AtomicLong and LongAdder do, wrapping on overflow.
 * Signed overflow is undefined in C, so add in uint64_t. */
static int64_t wrap_add(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

/* Read-modify-write in one critical section: this is the whole point of
 * these methods over a get/set pair. */
static av_type *al_add_and_get(av_type *self, environment *env, av_list *args, int64_t delta)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    s->ival = wrap_add(s->ival, delta);
    int64_t v = s->ival;
    pthread_mutex_unlock(&s->lock);
    (void)args;
    return &av_int_new(v)->base.base;
}

static av_type *al_incrementAndGet(av_type *self, environment *env, av_list *args)
{ return al_add_and_get(self, env, args, 1); }

static av_type *al_decrementAndGet(av_type *self, environment *env, av_list *args)
{ return al_add_and_get(self, env, args, -1); }

static av_type *al_addAndGet(av_type *self, environment *env, av_list *args)
{ return al_add_and_get(self, env, args, arg_i(args, 0)); }

static av_type *al_getAndSet(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    int64_t old = s->ival;
    s->ival = arg_i(args, 0);
    pthread_mutex_unlock(&s->lock);
    return &av_int_new(old)->base.base;
}

static av_type *al_compareAndSet(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    int64_t expect = arg_i(args, 0), update = arg_i(args, 1);
    pthread_mutex_lock(&s->lock);
    bool ok = (s->ival == expect);
    if (ok) s->ival = update;
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(ok)->base.base;
}

/* --- AtomicBool --------------------------------------------------- */

static av_type *ab_new(av_type *self, environment *env, av_list *args)
{
    (void)env;
    state_new(self)->bval = arg_b(args, 0);
    return self;
}

static av_type *ab_get(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    bool v = s->bval;
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(v)->base.base;
}

static av_type *ab_set(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    s->bval = arg_b(args, 0);
    pthread_mutex_unlock(&s->lock);
    return self;
}

static av_type *ab_getAndSet(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    bool old = s->bval;
    s->bval = arg_b(args, 0);
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(old)->base.base;
}

static av_type *ab_compareAndSet(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    bool expect = arg_b(args, 0), update = arg_b(args, 1);
    pthread_mutex_lock(&s->lock);
    bool ok = (s->bval == expect);
    if (ok) s->bval = update;
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(ok)->base.base;
}

/* --- AtomicRef ---------------------------------------------------- */

static av_type *ar_new(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = state_new(self);
    if (av_list_size(args) > 0) s->ref = av_list_get(args, 0);
    return self;
}

static av_type *ar_get(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    av_type *v = s->ref;
    pthread_mutex_unlock(&s->lock);
    return v;
}

static av_type *ar_set(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    s->ref = av_list_get(args, 0);
    pthread_mutex_unlock(&s->lock);
    return self;
}

static av_type *ar_getAndSet(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    av_type *old = s->ref;
    s->ref = av_list_get(args, 0);
    pthread_mutex_unlock(&s->lock);
    return old;
}

static av_type *ar_compareAndSet(av_type *self, environment *env, av_list *args)
{
    (void)env;
    conc_state *s = self_state(self);
    av_type *expect = av_list_get(args, 0);
    av_type *update = av_list_get(args, 1);
    pthread_mutex_lock(&s->lock);
    /* Identity match, except null-to-null which matches by kind because
     * every script null literal is a distinct null instance. */
    bool ok = (s->ref == expect)
        || (s->ref->type == C_TYPE_NULL && expect->type == C_TYPE_NULL);
    if (ok) s->ref = update;
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(ok)->base.base;
}

/* --- Counter ------------------------------------------------------
 * Java backs this with LongAdder, which trades exact intermediate reads
 * for write throughput under heavy contention. A plain guarded counter
 * has the same observable semantics for the operations exposed here. */

/* Counter has no explicit constructor in concurrent.aus, so its state
 * is created on first use. self_state now does exactly that (create if
 * absent, validated by the magic tag), which also gives a Counter
 * subclass its own state instead of corrupting the parent, so this is a thin alias kept for call-site clarity. */
static conc_state *counter_state(av_type *self) { return self_state(self); }

static av_type *cnt_add(av_type *self, environment *env, av_list *args, int64_t delta)
{
    (void)env; (void)args;
    conc_state *s = counter_state(self);
    pthread_mutex_lock(&s->lock);
    s->ival = wrap_add(s->ival, delta);
    pthread_mutex_unlock(&s->lock);
    return self;
}

static av_type *cnt_increment(av_type *self, environment *env, av_list *args)
{ return cnt_add(self, env, args, 1); }

static av_type *cnt_decrement(av_type *self, environment *env, av_list *args)
{ return cnt_add(self, env, args, -1); }

static av_type *cnt_addDelta(av_type *self, environment *env, av_list *args)
{ return cnt_add(self, env, args, arg_i(args, 0)); }

static av_type *cnt_sum(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = counter_state(self);
    pthread_mutex_lock(&s->lock);
    int64_t v = s->ival;
    pthread_mutex_unlock(&s->lock);
    return &av_int_new(v)->base.base;
}

static av_type *cnt_reset(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = counter_state(self);
    pthread_mutex_lock(&s->lock);
    s->ival = 0;
    pthread_mutex_unlock(&s->lock);
    return self;
}

/* --- BlockingQueue ------------------------------------------------ */

static av_type *bq_new(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t cap = arg_i(args, 0);
    av_type *err = stdlib_check_int_range("BlockingQueue", cap);
    if (err != NULL) return err;
    conc_state *s = state_new(self);
    s->queue = av_vec_new();
    s->capacity = (cap > 0) ? cap : 0;   /* 0 = unbounded */
    return self;
}

static av_type *bq_offer(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_type *val = av_list_get(args, 0);
    if (val->type == C_TYPE_NULL) {
        return conc_exc("BlockingQueue.offer(): null values cannot be queued.");
    }
    int64_t timeout = arg_i(args, 1);
    conc_state *s = self_state(self);

    pthread_mutex_lock(&s->lock);
    bool queued = false;
    if (s->capacity == 0 || av_vec_size(s->queue) < (size_t)s->capacity) {
        av_vec_add(s->queue, val);
        queued = true;
    } else if (timeout > 0) {
        struct timespec ts;
        deadline_in(timeout, &ts);
        while (av_vec_size(s->queue) >= (size_t)s->capacity) {
            if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT) break;
        }
        if (av_vec_size(s->queue) < (size_t)s->capacity) {
            av_vec_add(s->queue, val);
            queued = true;
        }
    }
    if (queued) pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(queued)->base.base;
}

static av_type *bq_put(av_type *self, environment *env, av_list *args)
{
    (void)env;
    av_type *val = av_list_get(args, 0);
    if (val->type == C_TYPE_NULL) {
        return conc_exc("BlockingQueue.put(): null values cannot be queued.");
    }
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    while (s->capacity != 0 && av_vec_size(s->queue) >= (size_t)s->capacity) {
        pthread_cond_wait(&s->cond, &s->lock);
    }
    av_vec_add(s->queue, val);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return self;
}

/* Removes and returns the head; caller holds the state lock. */
static av_type *queue_pop_locked(conc_state *s)
{
    return av_vec_remove_at(s->queue, 0);
}

static av_type *bq_poll(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t timeout = arg_i(args, 0);
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    if (av_vec_size(s->queue) == 0 && timeout > 0) {
        struct timespec ts;
        deadline_in(timeout, &ts);
        while (av_vec_size(s->queue) == 0) {
            if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT) break;
        }
    }
    av_type *ret;
    if (av_vec_size(s->queue) == 0) {
        ret = &av_null_new()->base.base;
    } else {
        ret = queue_pop_locked(s);
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static av_type *bq_take(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    while (av_vec_size(s->queue) == 0) {
        pthread_cond_wait(&s->cond, &s->lock);
    }
    av_type *ret = queue_pop_locked(s);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static av_type *bq_size(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    int64_t n = (int64_t)av_vec_size(s->queue);
    pthread_mutex_unlock(&s->lock);
    return &av_int_new(n)->base.base;
}

static av_type *bq_clear(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    while (av_vec_size(s->queue) > 0) av_vec_remove_at(s->queue, av_vec_size(s->queue) - 1);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return self;
}

/* --- Semaphore ---------------------------------------------------- */

static av_type *sem_new(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t permits = (av_list_size(args) > 0) ? arg_i(args, 0) : 1;
    av_type *err = stdlib_check_int_range("Semaphore", permits);
    if (err != NULL) return err;
    conc_state *s = state_new(self);
    s->ival = permits;
    return self;
}

static av_type *sem_tryAcquire(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t timeout = arg_i(args, 0);
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    if (s->ival <= 0 && timeout > 0) {
        struct timespec ts;
        deadline_in(timeout, &ts);
        while (s->ival <= 0) {
            if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT) break;
        }
    }
    bool got = (s->ival > 0);
    if (got) s->ival--;
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(got)->base.base;
}

static av_type *sem_release(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    if (s->ival >= INT32_MAX) {
        /* Java's Semaphore holds an int count and throws an Error past
         * its maximum, which ASemaphore reports. */
        pthread_mutex_unlock(&s->lock);
        return conc_exc("Semaphore.release(): Permit count is already at the "
                        "maximum int value 2147483647.");
    }
    s->ival++;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return self;
}

static av_type *sem_availablePermits(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    int64_t v = s->ival;
    pthread_mutex_unlock(&s->lock);
    return &av_int_new(v)->base.base;
}

/* --- Latch -------------------------------------------------------- */

static av_type *latch_new(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t count = (av_list_size(args) > 0) ? arg_i(args, 0) : 1;
    av_type *err = stdlib_check_int_range("Latch", count);
    if (err != NULL) return err;
    if (count < 0) return conc_exc("Latch(): Count must not be negative.");
    conc_state *s = state_new(self);
    s->ival = count;
    return self;
}

static av_type *latch_countDown(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    if (s->ival > 0) s->ival--;
    if (s->ival == 0) pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return self;
}

static av_type *latch_await(av_type *self, environment *env, av_list *args)
{
    (void)env;
    int64_t timeout = arg_i(args, 0);
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    if (s->ival > 0 && timeout > 0) {
        struct timespec ts;
        deadline_in(timeout, &ts);
        while (s->ival > 0) {
            if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT) break;
        }
    }
    bool open = (s->ival == 0);
    pthread_mutex_unlock(&s->lock);
    return &av_bool_new(open)->base.base;
}

static av_type *latch_getCount(av_type *self, environment *env, av_list *args)
{
    (void)env; (void)args;
    conc_state *s = self_state(self);
    pthread_mutex_lock(&s->lock);
    int64_t v = s->ival;
    pthread_mutex_unlock(&s->lock);
    return &av_int_new(v)->base.base;
}

/* --- Lock ---------------------------------------------------------
 * Reentrant, matching Java's ReentrantLock: the callback may itself
 * call withLock on the same Lock without deadlocking. Modelled with a
 * mutex + condvar rather than a recursive mutex, because the lock must
 * be released around the callback's own blocking and re-acquired on the
 * same thread. */

/* Lock has no explicit constructor either; same as counter_state. */
static conc_state *lock_state(av_type *self) { return self_state(self); }

static av_type *lock_withLock(av_type *self, environment *env, av_list *args)
{
    if (av_list_size(args) == 0 || av_list_get(args, 0)->type != C_TYPE_CALLBACK) {
        return conc_exc("Lock.withLock(): expecting a callback as the first argument.");
    }
    av_callback *cb = av_list_get_callback(args, 0);
    if (cb == NULL) return conc_exc("lock_withLock: argument 1 must be a callback.");
    int64_t wait_ms = (av_list_size(args) > 1) ? arg_i(args, 1) : 30000;

    conc_state *s = lock_state(self);
    pthread_t me = pthread_self();

    pthread_mutex_lock(&s->lock);
    bool acquired = false;
    if (s->owned && pthread_equal(s->owner, me)) {
        s->lock_depth++;             /* reentrant: already ours */
        acquired = true;
    } else {
        if (s->owned && wait_ms > 0) {
            struct timespec ts;
            deadline_in(wait_ms, &ts);
            while (s->owned) {
                if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT) break;
            }
        }
        if (!s->owned) {
            s->owned = true;
            s->owner = me;
            s->lock_depth = 1;
            acquired = true;
        }
    }
    pthread_mutex_unlock(&s->lock);

    if (!acquired) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "Lock.withLock(): timed out after %lld ms waiting for the lock. "
            "Possible deadlock or a slow critical section.", (long long)wait_ms);
        return conc_exc(msg);
    }

    /* The callback runs OUTSIDE the state mutex, holding only the
     * logical lock, so other threads can still query the object. */
    av_type *ret = callback_call(cb, env, av_list_new());

    pthread_mutex_lock(&s->lock);
    if (--s->lock_depth <= 0) {
        s->lock_depth = 0;
        s->owned = false;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

/* --- registration -------------------------------------------------- */

const aussom_extern_method stdlib_aatomic_long_methods[] = {
    { "newAtomicLong", al_new },
    { "get", al_get },
    { "set", al_set },
    { "incrementAndGet", al_incrementAndGet },
    { "decrementAndGet", al_decrementAndGet },
    { "addAndGet", al_addAndGet },
    { "getAndSet", al_getAndSet },
    { "compareAndSet", al_compareAndSet },
    { NULL, NULL }
};

const aussom_extern_method stdlib_aatomic_bool_methods[] = {
    { "newAtomicBool", ab_new },
    { "get", ab_get },
    { "set", ab_set },
    { "getAndSet", ab_getAndSet },
    { "compareAndSet", ab_compareAndSet },
    { NULL, NULL }
};

const aussom_extern_method stdlib_aatomic_ref_methods[] = {
    { "newAtomicRef", ar_new },
    { "get", ar_get },
    { "set", ar_set },
    { "getAndSet", ar_getAndSet },
    { "compareAndSet", ar_compareAndSet },
    { NULL, NULL }
};

const aussom_extern_method stdlib_acounter_methods[] = {
    { "increment", cnt_increment },
    { "decrement", cnt_decrement },
    { "add", cnt_addDelta },
    { "sum", cnt_sum },
    { "reset", cnt_reset },
    { NULL, NULL }
};

const aussom_extern_method stdlib_ablocking_queue_methods[] = {
    { "newBlockingQueue", bq_new },
    { "offer", bq_offer },
    { "poll", bq_poll },
    { "put", bq_put },
    { "take", bq_take },
    { "size", bq_size },
    { "clear", bq_clear },
    { NULL, NULL }
};

const aussom_extern_method stdlib_asemaphore_methods[] = {
    { "newSemaphore", sem_new },
    { "tryAcquire", sem_tryAcquire },
    { "release", sem_release },
    { "availablePermits", sem_availablePermits },
    { NULL, NULL }
};

const aussom_extern_method stdlib_alatch_methods[] = {
    { "newLatch", latch_new },
    { "countDown", latch_countDown },
    { "await", latch_await },
    { "getCount", latch_getCount },
    { NULL, NULL }
};

const aussom_extern_method stdlib_alock_methods[] = {
    { "withLock", lock_withLock },
    { NULL, NULL }
};
