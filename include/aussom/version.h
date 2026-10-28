/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The aussom-c version.
 *
 * This is aussom-c's own release number, not aussom-base's: the two are
 * released separately, and the embedding API in these headers has no
 * counterpart in aussom-base. The README says which aussom-base release
 * this one matches.
 *
 * The three numbers below are the only place the version is written. The
 * Makefile reads them, the command line's --help shows them, and
 * sys.getAussomVersion() returns them through aussom_version(), so a
 * release is one edit.
 *
 * They are macros rather than an enum because an enum constant is
 * invisible to the preprocessor: a host writing
 *
 *     #if AUSSOM_VERSION_MAJOR >= 1
 *
 * against an enum gets the name treated as zero, with no warning.
 *
 * These say what a host was compiled against. aussom_version() says
 * what is running, and once the library can be linked as a shared
 * library the two need not agree.
 *
 * While the major number is zero nothing is promised: the interface may
 * change in any release, and a host should pin an exact version.
 */

#ifndef AUSSOM_VERSION_H
#define AUSSOM_VERSION_H

/* Changes when something that existed is changed or taken away. */
#define AUSSOM_VERSION_MAJOR 0
/* Changes when something is added and nothing existing moves. */
#define AUSSOM_VERSION_MINOR 3
/* Changes when only the implementation did. */
#define AUSSOM_VERSION_PATCH 1

/* What follows the numbers, such as "-alpha". Empty for a release. */
#define AUSSOM_VERSION_LABEL "-alpha"

/* Turning a macro's value into a string takes two steps: the inner one
 * expands the argument, the outer one quotes the result. One step alone
 * quotes the name. Not part of the interface. */
#define AUSSOM_QUOTE_(x) #x
#define AUSSOM_QUOTE(x)  AUSSOM_QUOTE_(x)

/* The same version as text, with the label on the end, for showing and
 * logging. Built from the three numbers rather than written out again,
 * so the two cannot disagree. */
#define AUSSOM_VERSION_STRING \
    AUSSOM_QUOTE(AUSSOM_VERSION_MAJOR) "." \
    AUSSOM_QUOTE(AUSSOM_VERSION_MINOR) "." \
    AUSSOM_QUOTE(AUSSOM_VERSION_PATCH) AUSSOM_VERSION_LABEL

/* The version of the library that is actually linked in: a string owned
 * by the library, never NULL. */
const char *aussom_version(void);

#endif /* AUSSOM_VERSION_H */
