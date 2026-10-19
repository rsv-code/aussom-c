/*
 * Copyright 2026 Austin Lehman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * The static `sys` class (com.aussom.stdlib.ASys). Provides system,
 * path and time information.
 *
 * Each getter is gated on a security-manager property (see ASys.java
 * and security_manager.c): os/java/path/user views all default to
 * false, so by default these return null -- matching the Java CLI's
 * DefaultSecurityManager. getMills / sleep / getSysInfo are not gated
 * (getSysInfo relies on the per-field gating of the getters it calls).
 *
 * The Java-runtime getters (getJavaVersion / getJavaVendor /
 * getJavaVendorUrl / getJavaClassPath) have no meaning in a C build
 * and return null even when permitted. getJavaHome still reports
 * $JAVA_HOME, matching the Java System.getenv call.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* Windows has neither <pwd.h> nor <sys/utsname.h>. The three OS
 * information methods and the two user methods below are the only
 * places that need them, and each has a Win32 equivalent.
 *
 * Apple needs the POSIX set as well, not instead: it takes the POSIX
 * branch at every call site below except exe_path, and only that one
 * needs <mach-o/dyld.h>. Keep this block's branches matched to the
 * guards at the call sites; an earlier version replaced the POSIX
 * includes here rather than adding to them, which built on Linux and
 * failed on every Apple target. */
#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  define getcwd _getcwd
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>       /* _NSGetExecutablePath */
#  include <pwd.h>
#  include <stdint.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#else
#  include <pwd.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

#include "aussom/memstream.h"
#include "aussom/ast_eval.h"
#include "aussom/av.h"
#include "aussom/aussom_gc.h"
#include "aussom/engine.h"
#include "aussom/security_manager.h"
#include "aussom/extern_registry.h"
#include "aussom/version.h"
#include "stdlib_internal.h"

static av_type *snull(void) { return &av_null_new()->base.base; }
static av_type *sstr(const char *s) { return &av_string_new(s != NULL ? s : "")->base.base; }

/* Security gate: true when the calling engine permits `prop`. */
static bool allows(environment *e, const char *prop)
{
    return security_manager_get_property_boolean(engine_get_security_manager(environment_engine(e)), prop, false);
}

/* --- OS information ---------------------------------------------- */

#if defined(_WIN32)

/* uname has no Win32 equivalent, so each field is answered from the
 * closest API. The strings differ from a POSIX uname by necessity;
 * they answer the same questions. */
static av_type *sys_getOsArch(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "os.info.view")) return snull();
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return sstr("x86_64");
        case PROCESSOR_ARCHITECTURE_ARM64: return sstr("aarch64");
        case PROCESSOR_ARCHITECTURE_ARM:   return sstr("arm");
        case PROCESSOR_ARCHITECTURE_INTEL: return sstr("x86");
        default:                           return sstr("unknown");
    }
}

static av_type *sys_getOsName(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "os.info.view")) return snull();
    return sstr("Windows");
}

static av_type *sys_getOsVersion(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "os.info.view")) return snull();
    /* Reported as major.minor, the same shape as a POSIX uname release.
     * Note that Windows caps this at 6.2 for a process without an
     * application manifest declaring newer support; that is Windows
     * behaviour, not a defect here, and it needs no extra library. */
    DWORD v = GetVersion();
    char buf[64];
    snprintf(buf, sizeof buf, "%u.%u",
             (unsigned)(LOBYTE(LOWORD(v))), (unsigned)(HIBYTE(LOWORD(v))));
    return sstr(buf);
}

#else

static av_type *sys_getOsArch(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "os.info.view")) return snull();
    struct utsname u;
    return uname(&u) == 0 ? sstr(u.machine) : snull();
}

static av_type *sys_getOsName(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "os.info.view")) return snull();
    struct utsname u;
    return uname(&u) == 0 ? sstr(u.sysname) : snull();
}

static av_type *sys_getOsVersion(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "os.info.view")) return snull();
    struct utsname u;
    return uname(&u) == 0 ? sstr(u.release) : snull();
}

#endif

/* These answered "/" and "\n" unconditionally, which is right
 * everywhere except Windows, where it compiled cleanly and returned the
 * wrong answer. */
#if defined(_WIN32)
#  define AUSSOM_FILE_SEPARATOR "\\"
#  define AUSSOM_LINE_SEPARATOR "\r\n"
#else
#  define AUSSOM_FILE_SEPARATOR "/"
#  define AUSSOM_LINE_SEPARATOR "\n"
#endif

static av_type *sys_getFileSeparator(av_type *s, environment *e, av_list *a)
{ (void)s; (void)a; return allows(e, "os.info.view") ? sstr(AUSSOM_FILE_SEPARATOR) : snull(); }

static av_type *sys_getLineSeparator(av_type *s, environment *e, av_list *a)
{ (void)s; (void)a; return allows(e, "os.info.view") ? sstr(AUSSOM_LINE_SEPARATOR) : snull(); }

/* --- Java information (no JVM in the C build) -------------------- */

static av_type *sys_getJavaVersion(av_type *s, environment *e, av_list *a)
{ (void)e; (void)s; (void)a; return snull(); }
static av_type *sys_getJavaVendor(av_type *s, environment *e, av_list *a)
{ (void)e; (void)s; (void)a; return snull(); }
static av_type *sys_getJavaVendorUrl(av_type *s, environment *e, av_list *a)
{ (void)e; (void)s; (void)a; return snull(); }
static av_type *sys_getJavaClassPath(av_type *s, environment *e, av_list *a)
{ (void)e; (void)s; (void)a; return snull(); }

static av_type *sys_getJavaHome(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "java.home.view")) return snull();
    const char *h = getenv("JAVA_HOME");
    return h != NULL ? sstr(h) : snull();
}

/* --- Aussom / assembly information ------------------------------- */

static av_type *sys_getAussomVersion(av_type *s, environment *e, av_list *a)
{ (void)s; (void)a; return allows(e, "aussom.info.view") ? sstr(aussom_version()) : snull(); }

/* The running executable's path. Every platform answers this
 * differently: /proc/self/exe is Linux (and Android), Darwin has
 * _NSGetExecutablePath, and Windows has GetModuleFileName. */
static char *exe_path(void)
{
    static char buf[4096];
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return NULL;
    return buf;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return NULL;
    return buf;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return buf;
#endif
}

static av_type *sys_getAssembly(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "aussom.info.view")) return snull();
    char *p = exe_path();
    return p != NULL ? sstr(p) : snull();
}

static av_type *sys_getAssemblyPath(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "aussom.path.view")) return snull();
    char *p = exe_path();
    if (p == NULL) return snull();
    char *slash = strrchr(p, '/');
    if (slash == NULL) return sstr("./");
    *(slash + 1) = '\0';   /* keep the trailing separator */
    return sstr(p);
}

/* --- user / path information ------------------------------------- */

/* Both check the environment first, as they always have. The fallback
 * is where the platforms part: Windows has no passwd database, and the
 * variables it does set are named differently. */
static const char *home_dir(void)
{
    const char *h = getenv("HOME");
    if (h != NULL) return h;
#if defined(_WIN32)
    return getenv("USERPROFILE");
#else
    struct passwd *pw = getpwuid(getuid());
    return pw != NULL ? pw->pw_dir : NULL;
#endif
}

static const char *user_name(void)
{
    const char *u = getenv("USER");
    if (u != NULL) return u;
#if defined(_WIN32)
    return getenv("USERNAME");
#else
    struct passwd *pw = getpwuid(getuid());
    return pw != NULL ? pw->pw_name : NULL;
#endif
}

static av_type *sys_getCurrentPath(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "current.path.view")) return snull();
    char buf[4096];
    return getcwd(buf, sizeof(buf)) != NULL ? sstr(buf) : snull();
}

static av_type *sys_getHomePath(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "home.path.view")) return snull();
    const char *h = home_dir();
    return h != NULL ? sstr(h) : snull();
}

static av_type *sys_getUserName(av_type *s, environment *e, av_list *a)
{
    (void)s; (void)a;
    if (!allows(e, "user.name.view")) return snull();
    const char *u = user_name();
    return u != NULL ? sstr(u) : snull();
}

/* --- combined report --------------------------------------------- */

static const char *strval(av_type *v)
{
    return v->type == C_TYPE_STRING ? av_string_get((av_string *)v) : "null";
}

static av_type *sys_getSysInfo(av_type *s, environment *e, av_list *a)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = aussom_memstream_open(&buf, &len);
    if (fp == NULL) return &av_exception_new("sys.getSysInfo(): out of memory.")->base.base;

    fputs("OS Information\n", fp);
    fprintf(fp, "\tOS Arch: %s\n", strval(sys_getOsArch(s, e, a)));
    fprintf(fp, "\tOS Name: %s\n", strval(sys_getOsName(s, e, a)));
    fprintf(fp, "\tOS Version: %s\n", strval(sys_getOsVersion(s, e, a)));
    fprintf(fp, "\tOS File Separator: %s\n", strval(sys_getFileSeparator(s, e, a)));

    fputs("\nJava Information\n", fp);
    fprintf(fp, "\tJava Version: %s\n", strval(sys_getJavaVersion(s, e, a)));
    fprintf(fp, "\tJava Vendor: %s\n", strval(sys_getJavaVendor(s, e, a)));
    fprintf(fp, "\tJava Vendor Url: %s\n", strval(sys_getJavaVendorUrl(s, e, a)));
    fprintf(fp, "\tJava Class Path: %s\n", strval(sys_getJavaClassPath(s, e, a)));
    fprintf(fp, "\tJava Home: %s\n", strval(sys_getJavaHome(s, e, a)));

    fputs("\nAussom Information\n", fp);
    fprintf(fp, "\tAussom Interpreter Version: %s\n", strval(sys_getAussomVersion(s, e, a)));
    fprintf(fp, "\tAussom Assembly: %s\n", strval(sys_getAssembly(s, e, a)));
    fprintf(fp, "\tAussom Assembly Path: %s\n", strval(sys_getAssemblyPath(s, e, a)));

    fputs("\nUser Information\n", fp);
    fprintf(fp, "\tCurrent Path: %s\n", strval(sys_getCurrentPath(s, e, a)));
    fprintf(fp, "\tHome Path: %s\n", strval(sys_getHomePath(s, e, a)));
    fprintf(fp, "\tUser Name: %s\n", strval(sys_getUserName(s, e, a)));
    fputc('\n', fp);

    aussom_memstream_close(fp, &buf, &len);
    char *out = AUSSOM_STRDUP(buf);
    aussom_memstream_free(buf);
    return sstr(out);
}

/* --- time -------------------------------------------------------- */

static av_type *sys_getMills(av_type *s, environment *e, av_list *a)
{
    (void)e; (void)s; (void)a;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return &av_int_new(ms)->base.base;
}

/* Sleeps `ms` milliseconds without checking anything. Retries on a
 * signal so a short interruption does not cut the wait short. A wait
 * longer than time_t holds (a 32-bit time_t is about 68 years of
 * seconds) is cut to the longest one, instead of wrapping to a negative
 * time that nanosleep refuses at once. */
static void sleep_ms(int64_t ms)
{
    const time_t tmax = (time_t)((sizeof(time_t) == 8) ? INT64_MAX : INT32_MAX);
    struct timespec ts;
    if (ms / 1000 > (int64_t)tmax) {
        ts.tv_sec = tmax;
        ts.tv_nsec = 999999999L;
    } else {
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    }
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* ts now holds the remaining time; keep waiting. */
    }
}

/* sleep(mills)
 *
 * The wait is broken into short slices rather than one long wait,
 * which is what lets the host stay in control of a sleeping program.
 * Between slices the interpreter reads the engine's control state, so:
 *
 *   - Cancelling a sleeping program ends the sleep and raises the
 *     execution cancelled exception, which a script cannot catch.
 *   - Pausing a sleeping program suspends it: the time remaining stops
 *     counting down until the program is resumed, so the program still
 *     gets the full sleep it asked for and simply finishes later than
 *     the wall clock would suggest.
 *
 * Slice length comes from the security property
 * aussom.limit.sleep.slice because it is a control setting and those
 * live with the rest of the policy. The default is 50 ms, which is the
 * longest a cancel or a pause waits to take effect. Setting it to 0
 * turns slicing off and restores a single wait that nothing can
 * interrupt.
 *
 * A negative number of milliseconds raises an exception rather than
 * failing inside the runtime. */
static av_type *sys_sleep(av_type *self, environment *e, av_list *a)
{
    int64_t ms = av_numeric_int(av_list_get(a, 0));
    if (ms < 0) {
        return &av_exception_new("sys.sleep(): Milliseconds must not be negative.")->base.base;
    }
    if (ms == 0) return self;

    engine *eng = environment_engine(e);
    int64_t slice = (eng != NULL) ? engine_get_limits(eng)->sleep_slice_ms : 0;

    if (slice <= 0) {
        /* Slicing off: one wait, uninterruptible by design. */
        sleep_ms(ms);
        return self;
    }

    int64_t left = ms;
    while (left > 0) {
        /* Checked before each slice, so a cancel that arrives while the
         * program is sleeping is seen within one slice. A pause blocks
         * inside this call, which is what stops the remaining time from
         * counting down. */
        av_type *stop = eval_check_control_env(e);
        if (stop != NULL) return stop;

        int64_t chunk = (left < slice) ? left : slice;
        sleep_ms(chunk);
        left -= chunk;
    }
    return self;   /* Java returns env.getClassInstance() */
}

const aussom_extern_method stdlib_asys_methods[] = {
    { "getSysInfo", sys_getSysInfo },
    { "getAssembly", sys_getAssembly },
    { "getAssemblyPath", sys_getAssemblyPath },
    { "getCurrentPath", sys_getCurrentPath },
    { "getHomePath", sys_getHomePath },
    { "getUserName", sys_getUserName },
    { "getOsArch", sys_getOsArch },
    { "getOsName", sys_getOsName },
    { "getOsVersion", sys_getOsVersion },
    { "getJavaVersion", sys_getJavaVersion },
    { "getJavaVendor", sys_getJavaVendor },
    { "getJavaVendorUrl", sys_getJavaVendorUrl },
    { "getJavaClassPath", sys_getJavaClassPath },
    { "getFileSeparator", sys_getFileSeparator },
    { "getLineSeparator", sys_getLineSeparator },
    { "getAussomVersion", sys_getAussomVersion },
    { "getJavaHome", sys_getJavaHome },
    { "getMills", sys_getMills },
    { "sleep", sys_sleep },
    { NULL, NULL }
};
