/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Include loading. Ports Engine.addInclude: an include is parsed into
 * the engine at the moment it is reached, in the middle of the parse of
 * the file that names it, so the classes it defines exist before the
 * parser reaches the next class. The parser is re-entrant (all parse
 * state lives in parser_ctx and the flex scanner handle), which is what
 * makes the nested parse safe.
 */

#include "aussom/engine.h"
#include "aussom/security_manager.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "aussom/aussom_gc.h"
#include "aussom/parser.h"

/* True when any component of `include_name` below `root` is a symbolic
 * link. Ports Engine.includeHasSymlink: the include name always uses
 * '/' separators, and each component is checked as the path is built
 * up, so a link anywhere along the way is caught rather than only at
 * the leaf. */
static bool include_has_symlink(const char *root, const char *include_name)
{
    char path[PATH_MAX];
    size_t len = strlen(root);
    if (len >= sizeof(path)) return true;   /* refuse what we cannot check */
    memcpy(path, root, len);
    path[len] = '\0';

    const char *p = include_name;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t part_len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
        if (part_len > 0) {
            if (len + part_len + 2 >= sizeof(path)) return true;
            if (len > 0 && path[len - 1] != '/') path[len++] = '/';
            memcpy(path + len, p, part_len);
            len += part_len;
            path[len] = '\0';

#if defined(_WIN32)
            /* Windows has no lstat. A symbolic link is a reparse point,
             * which GetFileAttributes reports directly. */
            DWORD attr = GetFileAttributesA(path);
            if (attr != INVALID_FILE_ATTRIBUTES
                && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#else
            struct stat st;
            if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) return true;
#endif
        }
        if (slash == NULL) break;
        p = slash + 1;
    }
    return false;
}

/* True when this engine has already loaded `key`. Ports the
 * includes.contains checks in Engine.addInclude. */
static bool include_loaded(const engine *e, const char *key)
{
    for (size_t i = 0; i < e->includes_count; i++) {
        if (strcmp(e->includes[i], key) == 0) return true;
    }
    return false;
}

/* Records `key` as loaded. Done before the parse, as Java does, so an
 * include that names itself, directly or through another, is not
 * parsed again. */
static void record_include(engine *e, const char *key)
{
    if (e->includes_count == e->includes_capacity) {
        size_t new_cap = (e->includes_capacity == 0) ? 4 : e->includes_capacity * 2;
        e->includes = AUSSOM_REALLOC(e->includes, new_cap * sizeof(*e->includes));
        e->includes_capacity = new_cap;
    }
    e->includes[e->includes_count++] = AUSSOM_STRDUP(key);
}

static const char *fail(const char *message)
{
    return AUSSOM_STRDUP(message);
}

const char *engine_add_include(engine *e, const char *name)
{
    engine_clear_parse_exception(e);

    /* The engine's own modules first: the base library and whatever a
     * host added. Recorded under the include name. */
    const char *src = engine_module_source(e, name);
    if (src != NULL) {
        if (!include_loaded(e, name)) {
            record_include(e, name);
            /* Java parses a module under its resource path, which is
             * the file name its classes and stack frames carry. */
            static const char prefix[] = "/com/aussom/stdlib/aus/";
            size_t n = sizeof(prefix) + strlen(name);
            char *file_name = AUSSOM_ALLOC_ATOMIC(n);
            snprintf(file_name, n, "%s%s", prefix, name);
            aussom_parse_string(e, src, file_name);
        }
        return NULL;
    }

    /* Then each include path, recorded under the path it was found at.
     * Ports the includePaths loop, including the exclude-path refusal,
     * which applies whether or not the file exists there, and the
     * symlink control. */
    char msg[PATH_MAX + 128];
    size_t n = engine_include_path_count(e);
    for (size_t i = 0; i < n; i++) {
        const char *root = engine_get_include_path(e, i);

        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s%s", root, name);
        if (written < 0 || (size_t)written >= sizeof(candidate)) continue;

        if (engine_is_path_exclude_path(e, candidate)) {
            snprintf(msg, sizeof(msg),
                "Attempting to add include '%s' from excluded path.",
                candidate);
            return fail(msg);
        }

        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        /* Default true, matching the security manager default: an
         * include may be reached through a symbolic link unless a host
         * says otherwise. */
        if (!security_manager_get_property_boolean(engine_get_security_manager(e), "aussom.include.symlink.follow", true)
            && include_has_symlink(root, name)) {
            snprintf(msg, sizeof(msg),
                "Attempting to add include '%s' through a symbolic link, "
                "which policy does not allow.", candidate);
            return fail(msg);
        }

        if (!include_loaded(e, candidate)) {
            record_include(e, candidate);
            /* aussom_parse_file applies the source byte limit before it
             * reads, so that check is already carried here. */
            aussom_parse_file(e, candidate);
        }
        return NULL;
    }

    /* No search path held the module. Ports the `if (!found)` tail of
     * Engine.addInclude: an include that resolves to nothing is a
     * failure, not a silent no-op.
     *
     * Java suppresses this one refusal in EngineRunMode.DOC. The C
     * port has no run mode yet (aussomdoc generation is not
     * implemented), so there is nothing to suppress on; carry that
     * condition here when doc mode is ported. */
    snprintf(msg, sizeof(msg),
        "Engine.addInclude(): Couldn't find requested include module '%s'.",
        name);
    return fail(msg);
}
