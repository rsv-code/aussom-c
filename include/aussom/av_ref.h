/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef AUSSOM_AV_REF_H
#define AUSSOM_AV_REF_H

#include <stdbool.h>
#include <stdint.h>

#include "aussom/av_strmap.h"
#include "aussom/av_type.h"
#include "aussom/av_vec.h"

/* Mirrors com.aussom.types.AussomRef (cRef): an assignable handle to
 * a slot inside a list (by index) or a map / object members (by
 * key). The assignment path goes through the target collection's own
 * lock, so it inherits the object-level thread-safety -- the same
 * effect as AussomRef.assign's synchronized blocks on the Java side. */
/* Forward decl; the index-ref target is an object value. */
struct av_object;

typedef struct av_ref {
    av_type base;
    bool is_map;
    int64_t lkey;       /* list index when !is_map */
    char *mkey;         /* map key when is_map */
    av_vec *lobj;       /* list target when !is_map */
    av_strmap *mobj;    /* map/members target when is_map */

    /* Overloaded-index write target (= com.aussom.types.AussomIndexRef).
     * Set when obj[key] appears on the left of an assignment and obj's
     * class defines __opIndexSet__. aussom-base models this as a
     * subclass distinguished by instanceof; this port dispatches refs by
     * the C_TYPE_REF tag, so the subclass becomes a flag plus the two
     * extra fields it carries.
     *
     * This is a plain data holder: assigning through it is a deliberate
     * no-op, because the write must call __opIndexSet__, which needs the
     * environment. eval_assignment performs that call so runtime
     * exceptions from the method can propagate. */
    bool is_index;
    struct av_object *itarget;
    av_type *ikey;
} av_ref;

av_ref *av_ref_new(void);

/* Builds an overloaded-index write target for `target[key]`. */
av_ref *av_index_ref_new(struct av_object *target, av_type *key);

/* Point this ref at list slot `index` of `lobj`. */
void av_ref_set_list(av_ref *r, int64_t index, av_vec *lobj);

/* Point this ref at key `key` of map/members `mobj`. The key is
 * copied. */
void av_ref_set_map(av_ref *r, const char *key, av_strmap *mobj);

/* Reads the referenced value, or NULL when the slot/key is absent or
 * out of bounds. */
av_type *av_ref_get_value(const av_ref *r);

/* Writes `value` into the referenced slot/key (creating the key for
 * maps). Returns false only for an out-of-bounds list index. */
bool av_ref_assign(av_ref *r, av_type *value);

#endif /* AUSSOM_AV_REF_H */
