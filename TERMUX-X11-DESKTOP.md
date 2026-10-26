# Termux:X11 desktop and Prism status

This note records the 2026-10-26 phone session on the Samsung SM-A032F. It
documents the desktop/launcher state separately from the Mesa and vendor-GLES
results in [TESTING.md](TESTING.md). A visible XFCE desktop is not evidence
that XFCE or Minecraft is GPU-accelerated.

## Current state

- Termux:X11 display `:1` is running on the phone.
- Kali's already-installed XFCE session, window manager, panel, and desktop are
  running on that display. No Kali packages were installed or removed for this
  session.
- Prism Launcher 11.1.1 is staged privately under
  `/opt/pvr-bridge/prism-stage` and is open on the XFCE desktop.
- The Prism instance named `1.12.2` is configured for demo mode. The demo-name
  prompt was reached, and a Java 21 launch was attempted after enabling Prism's
  Java-compatibility-check bypass and reducing the configured maximum heap to
  384 MiB.
- Minecraft 1.12.2 still exits before rendering. Its log says Java 8 is
  required; the Java 21 attempt ended with a Java security-provider stack trace
  and exit code 2. No Minecraft menu or gameplay was verified.
- The separate `Canary` profile targets Minecraft 1.21.11, but Prism restores
  `-XX:+UseZGC`, which this ARMHF Java 21 runtime does not support. That profile
  is also not verified working.
- No Microsoft account was added and no credentials were handled.

## Starting XFCE on Termux:X11

Start the Termux:X11 Android activity and make sure its X server is display
`:1`. From an Android root shell, start the existing XFCE session inside the
Kali chroot with a chroot-local temporary directory. `TMPDIR=/tmp` matters:
without it, XFCE may try to create ICE session files under Android's
`/data/local/tmp` and fail to complete session startup.

```sh
am start -n com.termux.x11/.MainActivity
CH=/data/local/nhsystem/kali-armhf
export CH
su -c "mkdir -p \$CH/tmp/xfce-runtime; \
  chmod 700 \$CH/tmp/xfce-runtime; \
  chroot \$CH /usr/bin/env DISPLAY=:1 HOME=/root TMPDIR=/tmp \
    XDG_CONFIG_HOME=/root/.config XDG_DATA_HOME=/root/.local/share \
    XDG_RUNTIME_DIR=/tmp/xfce-runtime PATH=/usr/bin:/bin \
    /usr/bin/dbus-run-session -- /usr/bin/startxfce4 \
    >/tmp/xfce-x11.log 2>&1 &"
```

The command assumes the Termux:X11 server and the Kali chroot are already
available. If `startxfce4` reports that an X server is already running on
`:1`, that is expected when targeting the existing Termux:X11 server.

The phone's saved XFCE panel configuration contained an unavailable `null`
plugin. XFCE asked whether to remove it; removing that invalid panel entry let
the panel and desktop appear. XFCE also reports that the system bus/systemd and
some Android hardware applets are unavailable in this chroot. These warnings
did not prevent the desktop from appearing.

## Moving Prism when its title bar is obscured

The XFCE top panel initially overlapped Prism's title bar, making the window
hard to drag. Once the title bar is visible, drag it normally. XFCE's
Alt+left-drag window move gesture is another option when decorations are
difficult to reach. To reposition Prism from an Android root shell, find its
window ID and move the main (800x600) window below the panel. IDs change between
launches:

```sh
CH=/data/local/nhsystem/kali-armhf
export CH
W=$(su -c "chroot \$CH /usr/bin/env DISPLAY=:1 /usr/bin/xdotool search --onlyvisible --name 'Prism Launcher 11.1.1' | tail -n 1")
su -c "chroot \$CH /usr/bin/env DISPLAY=:1 /usr/bin/xdotool windowmove $W 100 70"
```

## Minecraft blockers and next steps

1. Stage a compatible ARMHF Java 8 runtime privately for the `1.12.2` instance;
   do not install it into Kali or publish its binaries in this repository.
2. Keep the heap within available phone memory. Prism's Java tab has dedicated
   minimum/maximum memory controls; do not put `-Xms`/`-Xmx` in the extra JVM
   arguments, because Prism warns about that configuration.
3. Retry `1.12.2` using Prism's **Play Demo** flow, then verify that the
   Minecraft window opens on `:1` before claiming success.
4. Investigate `Canary` separately: remove unsupported ZGC arguments through
   Prism's instance Java settings before testing its Java 21 launch.
5. Verify the actual renderer independently. XFCE's desktop appearance and
   vendor GLES benchmark output do not establish Minecraft compatibility,
   Mesa/Zink acceleration, or GPU-rendered XFCE.

## Privacy and repository boundary

Prism, Java runtimes, Minecraft libraries/assets, downloaded packages,
proprietary vendor libraries/firmware, account data, and device logs remain on
the phone or in private local staging. They are intentionally not included in
this repository. This document contains only setup/status notes and no
credentials or proprietary artifacts. Kali's package database and default
graphics stack were left unchanged during this desktop setup.
