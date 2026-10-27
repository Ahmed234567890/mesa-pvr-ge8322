/*
 * egl-bisect: skip individual EGL teardown calls, selected by environment.
 *
 * Used to find which call in the vendor EGL driver crashes on exit.  Preload
 * it in front of libhybris's libEGL and set one variable per run:
 *
 *   SKIP_TERMINATE=1        eglTerminate() returns EGL_TRUE
 *   SKIP_RELEASE_THREAD=1   eglReleaseThread() returns EGL_TRUE
 *   SKIP_DESTROY_SURFACE=1  eglDestroySurface() returns EGL_TRUE
 *   SKIP_DESTROY_CONTEXT=1  eglDestroyContext() returns EGL_TRUE
 *
 * Every other call is forwarded to the real implementation.  This is a
 * diagnostic helper, not a fix: skipping teardown leaks driver resources.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -fPIC -shared -o libegl-bisect.so egl-bisect.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>

typedef unsigned int EGLBoolean;
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;

static int skip(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && v[0] != '0';
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    static EGLBoolean (*real)(EGLDisplay);
    if (skip("SKIP_TERMINATE"))
        return 1;
    if (real == NULL)
        real = (EGLBoolean (*)(EGLDisplay))dlsym(RTLD_NEXT, "eglTerminate");
    return real ? real(dpy) : 1;
}

EGLBoolean eglReleaseThread(void)
{
    static EGLBoolean (*real)(void);
    if (skip("SKIP_RELEASE_THREAD"))
        return 1;
    if (real == NULL)
        real = (EGLBoolean (*)(void))dlsym(RTLD_NEXT, "eglReleaseThread");
    return real ? real() : 1;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    static EGLBoolean (*real)(EGLDisplay, EGLSurface);
    if (skip("SKIP_DESTROY_SURFACE"))
        return 1;
    if (real == NULL)
        real = (EGLBoolean (*)(EGLDisplay, EGLSurface))dlsym(RTLD_NEXT, "eglDestroySurface");
    return real ? real(dpy, surface) : 1;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    static EGLBoolean (*real)(EGLDisplay, EGLContext);
    if (skip("SKIP_DESTROY_CONTEXT"))
        return 1;
    if (real == NULL)
        real = (EGLBoolean (*)(EGLDisplay, EGLContext))dlsym(RTLD_NEXT, "eglDestroyContext");
    return real ? real(dpy, ctx) : 1;
}
