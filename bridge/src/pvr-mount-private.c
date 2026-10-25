// SPDX-License-Identifier: MIT
/*
 * pvr-mount-private - make a chroot bind-mount clone private.
 *
 * Android's toybox mount does not implement --make-rprivate (it fails
 * silently), which leaves recursive bind clones in the host's shared peer
 * group.  Unmounting such a clone propagates into the host namespace and can
 * detach /dev, including binderfs.
 *
 * This helper calls mount(MS_PRIVATE | MS_REC) directly.  Build it static so
 * it can be run from inside the chroot without the glibc runtime path being
 * available yet:
 *
 *   arm-none-linux-gnueabihf-gcc --sysroot="$SYSROOT" -static \
 *       -O2 -o pvr-mount-private pvr-mount-private.c
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    if (mount(NULL, argv[1], NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        fprintf(stderr, "make-rprivate %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
