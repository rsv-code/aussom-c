/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "aussom/av_ref.h"

#include "aussom/av_dispatch.h"
#include "aussom/aussom_gc.h"

av_ref *av_ref_new(void)
{
    av_ref *r = AUSSOM_ALLOC(sizeof(*r));
    r->base.type = C_TYPE_REF;
    r->is_map = true;
    r->lkey = -1;
    r->mkey = AUSSOM_STRDUP("");
    r->lobj = NULL;
    r->mobj = NULL;
    r->is_index = false;
    r->itarget = NULL;
    r->ikey = NULL;
    return r;
}

av_ref *av_index_ref_new(struct av_object *target, av_type *key)
{
    av_ref *r = av_ref_new();
    r->is_index = true;
    r->itarget = target;
    r->ikey = key;
    return r;
}

void av_ref_set_list(av_ref *r, int64_t index, av_vec *lobj)
{
    r->is_map = false;
    r->lkey = index;
    r->lobj = lobj;
    r->mkey = AUSSOM_STRDUP("");
    r->mobj = NULL;
}

void av_ref_set_map(av_ref *r, const char *key, av_strmap *mobj)
{
    r->is_map = true;
    r->mkey = AUSSOM_STRDUP(key);
    r->mobj = mobj;
    r->lkey = -1;
    r->lobj = NULL;
}

av_type *av_ref_get_value(const av_ref *r)
{
    if (r->is_map) {
        return (r->mobj != NULL) ? av_strmap_get(r->mobj, r->mkey) : NULL;
    }
    if (r->lobj == NULL || r->lkey < 0) return NULL;
    return av_vec_get(r->lobj, (size_t)r->lkey);
}

bool av_ref_assign(av_ref *r, av_type *value)
{
    /* An overloaded-index ref writes through __opIndexSet__, which
     * needs the environment; eval_assignment handles it. A plain assign
     * here would write into the orphaned backing map av_ref_new
     * allocates, so it is a deliberate no-op. */
    if (r->is_index) return true;
    if (r->is_map) {
        /* av_strmap_put takes the map's lock; creates the key when
         * absent. */
        av_strmap_put(r->mobj, r->mkey, value);
        return true;
    }
    /* av_vec_set takes the vec's lock; fails only out of bounds. */
    if (r->lkey < 0) return false;
    return av_vec_set(r->lobj, (size_t)r->lkey, value);
}

void av_ref_write(const av_type *v, int level, FILE *out)
{
    const av_ref *r = (const av_ref *)v;
    av_write_tabs(level, out);
    fputs("{\n", out);
    av_write_tabs(level + 1, out);
    fputs("\"type\": \"cRef\",\n", out);
    av_write_tabs(level + 1, out);
    if (r->is_map) {
        fprintf(out, "\"objType\": \"map\", \"mkey\": \"%s\"\n", r->mkey);
    } else {
        fprintf(out, "\"objType\": \"list\", \"lkey\": %lld\n", (long long)r->lkey);
    }
    av_write_tabs(level, out);
    fputc('}', out);
}

void av_ref_write_str(const av_type *v, int level, FILE *out)
{
    (void)level;
    const av_ref *r = (const av_ref *)v;
    fputs(r->is_map ? "cMap@ref" : "cList@ref", out);
}

av_type *av_ref_clone(const av_type *v)
{
    /* References are not deep-cloned; the Java AussomRef has no
     * clone override and is never user-visible as a cloneable value.
     * Return a shallow copy pointing at the same target. */
    const av_ref *r = (const av_ref *)v;
    av_ref *n = av_ref_new();
    n->is_map = r->is_map;
    n->lkey = r->lkey;
    n->mkey = AUSSOM_STRDUP(r->mkey);
    n->lobj = r->lobj;
    n->mobj = r->mobj;
    return &n->base;
}
