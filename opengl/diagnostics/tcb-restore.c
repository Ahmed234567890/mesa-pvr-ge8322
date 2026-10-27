/*
 * tcb-restore: restore glibc's thread-control block after the vendor EGL
 * driver has finished with a display.
 *
 * libhybris runs Android libraries on glibc.  Android's libc stores its
 * thread-local slots at fixed offsets from the thread pointer, and the vendor
 * PowerVR driver writes them (observed at tp+16, tp+32 and tp+52).  Those
 * offsets overlap glibc's own per-thread data, so the writes survive into
 * glibc's thread-exit path and can turn into wild jumps.
 *
 * This shim snapshots the first 16 words at the thread pointer before any
 * EGL call, then puts them back after eglTerminate() and eglReleaseThread().
 * At that point the driver is finished with the thread, so its TLS state is
 * no longer needed, while glibc needs its own back.
 *
 * Applications such as glmark2 do not link against libEGL at all: they
 * dlopen() it and look the entry points up with dlsym().  The shim therefore
 * intercepts dlsym() (and eglGetProcAddress()) and hands out its own
 * eglTerminate/eglReleaseThread wrappers.  Both entry points are also
 * interposed directly for applications that do link against libEGL.
 *
 * Diagnostic only.  The real fix belongs in libhybris's EGL wrappers.
 *
 * Status: this prototype did **not** fix the glmark2 teardown crash.  The
 * snapshot is taken in the shim's constructor, by which time glibc has already
 * left a sentinel (0xffffffff) in the slot the driver later overwrites, so the
 * restore puts back a value that is not the C-locale table either.  The crash
 * turned out to be a libhybris lifecycle bug; see nodlclose.c and
 * patches/libhybris-egl-terminate-order.patch.  The TLS aliasing itself is
 * real (see tls-probe.c) and still worth fixing for long-running processes.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -fPIC -shared -o libtcb-restore.so tcb-restore.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int EGLBoolean;
typedef void *EGLDisplay;

#define NWORDS 16

static void *raw_dlsym(void *handle, const char *name);

static uintptr_t saved[NWORDS];
static int have_saved;
static int restore_after_terminate = 1;
static int restore_after_release = 1;

static uintptr_t *tp(void)
{
    return (uintptr_t *)__builtin_thread_pointer();
}

static void save_once(void)
{
    if (!have_saved) {
        memcpy(saved, tp(), sizeof(saved));
        have_saved = 1;
    }
}

static void restore(const char *where)
{
    uintptr_t *t = tp();
    int i;
    int changed = 0;

    if (!have_saved)
        return;
    for (i = 0; i < NWORDS; i++) {
        if (t[i] != saved[i]) {
            fprintf(stderr, "tcb-restore[%s]: tcb[%d] 0x%08lx -> 0x%08lx\n",
                    where, i, (unsigned long)t[i], (unsigned long)saved[i]);
            t[i] = saved[i];
            changed = 1;
        }
    }
    if (!changed)
        fprintf(stderr, "tcb-restore[%s]: nothing to restore\n", where);
}

/* Resolved vendor-side entry points, filled in by eglGetProcAddress(). */
static EGLBoolean (*next_terminate)(EGLDisplay);
static EGLBoolean (*next_release)(void);

static EGLBoolean wrap_terminate(EGLDisplay dpy)
{
    EGLBoolean rc;

    save_once();
    rc = next_terminate ? next_terminate(dpy) : 1;
    if (restore_after_terminate)
        restore("terminate");
    return rc;
}

static EGLBoolean wrap_release(void)
{
    EGLBoolean rc;

    save_once();
    rc = next_release ? next_release() : 1;
    if (restore_after_release)
        restore("releaseThread");
    return rc;
}

void *eglGetProcAddress(const char *name)
{
    static void *(*real)(const char *);
    void *p;

    if (real == NULL)
        real = (void *(*)(const char *))raw_dlsym(RTLD_NEXT, "eglGetProcAddress");
    if (real == NULL)
        return NULL;

    p = real(name);
    if (p == NULL)
        return NULL;

    if (strcmp(name, "eglTerminate") == 0) {
        next_terminate = (EGLBoolean (*)(EGLDisplay))p;
        return (void *)wrap_terminate;
    }
    if (strcmp(name, "eglReleaseThread") == 0) {
        next_release = (EGLBoolean (*)(void))p;
        return (void *)wrap_release;
    }
    return p;
}

/*
 * glmark2 dlopen()s libEGL and resolves every entry point with dlsym() on
 * that handle, so a plain PLT interposer is never consulted.  Return the
 * wrappers from dlsym() as well.  dlvsym() is used to obtain the real dlsym()
 * so this lookup cannot recurse into the interposer itself.
 */
static void *(*real_dlsym)(void *, const char *);
static __thread int in_dlsym;
static __thread int raw_lookup;

/* Resolve a symbol without letting the interposer rewrite the result. */
static void *raw_dlsym(void *handle, const char *name)
{
    int saved = raw_lookup;
    void *p;

    if (real_dlsym == NULL)
        real_dlsym = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.4");
    if (real_dlsym == NULL)
        return NULL;
    raw_lookup = 1;
    p = real_dlsym(handle, name);
    raw_lookup = saved;
    return p;
}

void *dlsym(void *handle, const char *name)
{
    void *p;

    if (real_dlsym == NULL)
        real_dlsym = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.4");
    if (real_dlsym == NULL)
        return NULL;
    if (in_dlsym || raw_lookup)
        return real_dlsym(handle, name);

    in_dlsym = 1;
    p = real_dlsym(handle, name);
    in_dlsym = 0;

    if (p == NULL)
        return NULL;
    if (strcmp(name, "eglTerminate") == 0) {
        next_terminate = (EGLBoolean (*)(EGLDisplay))p;
        return (void *)wrap_terminate;
    }
    if (strcmp(name, "eglReleaseThread") == 0) {
        next_release = (EGLBoolean (*)(void))p;
        return (void *)wrap_release;
    }
    return p;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    static EGLBoolean (*real)(EGLDisplay);
    EGLBoolean rc;

    save_once();
    if (real == NULL)
        real = (EGLBoolean (*)(EGLDisplay))raw_dlsym(RTLD_NEXT, "eglTerminate");
    rc = real ? real(dpy) : 1;
    if (restore_after_terminate)
        restore("terminate-plt");
    return rc;
}

EGLBoolean eglReleaseThread(void)
{
    static EGLBoolean (*real)(void);
    EGLBoolean rc;

    save_once();
    if (real == NULL)
        real = (EGLBoolean (*)(void))raw_dlsym(RTLD_NEXT, "eglReleaseThread");
    rc = real ? real() : 1;
    if (restore_after_release)
        restore("releaseThread-plt");
    return rc;
}

__attribute__((constructor))
static void init(void)
{
    const char *v;

    v = getenv("TCB_RESTORE_TERMINATE");
    if (v != NULL && v[0] == '0')
        restore_after_terminate = 0;
    v = getenv("TCB_RESTORE_RELEASE");
    if (v != NULL && v[0] == '0')
        restore_after_release = 0;
    save_once();
}
