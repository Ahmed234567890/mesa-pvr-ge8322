/*
 * nodlclose: diagnostic shim that turns every dlclose() into a no-op.
 *
 * libhybris unloads its EGL platform module (eglplatform_x11.so) from
 * ws_Terminate(), which eglTerminate() calls.  If any callback registered
 * with the X server or the vendor driver still points into that module, the
 * next call jumps into unmapped memory.  Leaking the module keeps its code
 * mapped and tells us whether the unload is the cause.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -fPIC -shared -o libnodlclose.so nodlclose.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

int dlclose(void *handle)
{
    (void)handle;
    fprintf(stderr, "nodlclose: suppressed dlclose(%p)\n", handle);
    return 0;
}
