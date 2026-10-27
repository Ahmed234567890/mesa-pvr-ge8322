# EGL/GLES diagnostics

Small, self-contained probes used to separate problems in the vendor PowerVR
driver, libhybris, and the X11 path. None of them is installed in the Kali
package database; each is built on the host with the armhf toolchain and copied
into the chroot's `/tmp`.

Build environment used on this machine:

```sh
TC=$HOME/toolchains/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-linux-gnueabihf
SYS=$HOME/armhf-sysroot
LB=$HOME/mesa-pvr-build/libhybris
CC=$TC/bin/arm-none-linux-gnueabihf-gcc
```

## tls-probe.c

Snapshots the first 16 words at the thread pointer, runs a representative
EGL/GLES session, and prints every word the vendor driver changed. It proves
that the driver writes bionic TLS slots (`tp+16`, `tp+32`, `tp+52`) that alias
glibc's thread-local data.

```sh
$CC --sysroot=$SYS -O2 -Wall -Wextra -I$LB/hybris/include \
    -o tls-probe tls-probe.c \
    -L$LB/hybris/egl/.libs -L$LB/hybris/glesv2/.libs \
    -L$SYS/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,$LB/hybris/common/.libs \
    -lEGL -lGLESv2
```

Run it through `run-x11-gles`; it exits 0 and prints `CHANGED [stage] tcb[n]`.

## egl-bisect.c

Interposes `eglTerminate`, `eglReleaseThread`, `eglDestroySurface`, and
`eglDestroyContext`, skipping any one of them when the matching environment
variable is set (`SKIP_TERMINATE=1`, `SKIP_RELEASE_THREAD=1`, ...). Used to find
which teardown call triggers a crash.

```sh
$CC --sysroot=$SYS -O2 -Wall -Wextra -fPIC -shared \
    -o libegl-bisect.so egl-bisect.c -ldl
```

Note that an application which `dlopen()`s EGL and resolves symbols with
`dlsym()` bypasses this shim; `tcb-restore.c` shows how to intercept `dlsym()`
as well.

## tcb-restore.c

Saves the thread-control-block words and writes them back after
`eglTerminate()`/`eglReleaseThread()`. It intercepts `dlsym()` and
`eglGetProcAddress()` because glmark2 resolves EGL at runtime.

This prototype did not fix the teardown crash (the saved value is already a
sentinel), but it is the working example of how to wrap EGL entry points for an
application that does not link against libEGL.

```sh
$CC --sysroot=$SYS -O2 -Wall -Wextra -fPIC -shared \
    -o libtcb-restore.so tcb-restore.c -ldl
LD_PRELOAD=/tmp/libtcb-restore.so run-x11-gles glmark2-es2-bin ...
```

## nodlclose.c

Turns every `dlclose()` into a no-op. This is the probe that identified the
one-shot teardown crash: with it, 4/4 glmark2 runs exited 0 instead of
`rc=139`, which pointed at `ws_Terminate()` unloading `eglplatform_x11.so`
before the vendor `eglTerminate()` had finished with it.

```sh
$CC --sysroot=$SYS -O2 -Wall -Wextra -fPIC -shared \
    -o libnodlclose.so nodlclose.c -ldl
```

The real fix is `patches/libhybris-egl-terminate-order.patch`, not this shim.
