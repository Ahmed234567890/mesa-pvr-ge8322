/*
 * selinux-off: make libselinux report "disabled" inside a chroot that
 * inherits Android's SELinux policy.
 *
 * The Kali chroot runs under Android's SELinux domain (magisk/su).  Android's
 * policy has no D-Bus rules, so a session bus started in the chroot denies
 * every method call ("0 matched rules").  dbus-daemon only performs those
 * checks when is_selinux_enabled() says SELinux is on, so interpose it for
 * the desktop session.  This affects only the processes that preload it; it
 * does not change the Android policy or the chroot's files.
 *
 * Build (armhf):
 *   arm-none-linux-gnueabihf-gcc --sysroot=$SYSROOT -O2 -Wall -Wextra \
 *       -fPIC -shared -o libselinux-off.so selinux-off.c
 *
 * Usage:
 *   LD_PRELOAD=/opt/pvr-bridge/desktop/lib/libselinux-off.so \
 *     dbus-run-session -- startxfce4
 */
int is_selinux_enabled(void)
{
    return 0;
}

int selinuxfs_exists(void)
{
    return 0;
}
