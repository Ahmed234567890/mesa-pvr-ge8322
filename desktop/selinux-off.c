/*
 * selinux-off: make libselinux report "disabled" inside a chroot that
 * inherits Android's SELinux policy.
 *
 * The Kali chroot runs under Android's SELinux domain (magisk/su).  Android's
 * policy has no D-Bus rules, so a session bus started in the chroot denies
 * every method call ("0 matched rules").  dbus-daemon only performs those
 * checks when is_selinux_enabled() says SELinux is on, so interpose it for
 * the daemon.  This affects only the processes that preload it; it does not
 * change the Android policy or the chroot's files.
 *
 * The constructor clears LD_PRELOAD from the environment after the library is
 * loaded.  Without that, dbus-daemon passes LD_PRELOAD to every service it
 * activates, and those services then break any Android binary they start
 * (this shim is a glibc library).  The library stays mapped; only the
 * environment variable is removed.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -fPIC -shared -o libselinux-off.so selinux-off.c
 *
 * Usage (scope it to dbus-daemon only):
 *   LD_PRELOAD=.../libselinux-off.so dbus-daemon --session --print-address --fork
 */
#include <stdlib.h>

__attribute__((constructor))
static void drop_preload_from_environment(void)
{
    unsetenv("LD_PRELOAD");
}

int is_selinux_enabled(void)
{
    return 0;
}

int selinuxfs_exists(void)
{
    return 0;
}
