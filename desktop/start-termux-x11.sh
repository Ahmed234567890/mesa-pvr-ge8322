#!/system/bin/sh
# Start the Termux:X11 X server on display :1 without auth.
#
# This is the same command Termux's own `termux-x11` wrapper runs, reduced to
# the pieces that matter when starting it by hand as the app's uid:
#
#   adb shell su -c 'setsid su u0_a226 -c "sh /data/local/tmp/start-termux-x11.sh" \
#       >/dev/null 2>&1 &'
#
# TMPDIR must point at the Termux prefix tmp.  Without it the server logs
#   LorieNative: Using TMPDIR="/tmp"
#   LorieNative: Could not create lock file in ...
#   LorieNative: Server stopped (1)
# and no X socket is created, with nothing on the console to explain it.
#
# CLASSPATH selects the Termux:X11 loader APK that contains libXlorie.so.
# LD_LIBRARY_PATH/LD_PRELOAD are cleared the same way the Termux wrapper does,
# so an inherited glibc preload cannot reach Android's linker.
PREFIX=/data/data/com.termux/files/usr
export TMPDIR=$PREFIX/tmp
export CLASSPATH=$PREFIX/libexec/termux-x11/loader.apk
unset LD_LIBRARY_PATH LD_PRELOAD
cd /
exec /system/bin/app_process -Xnoimage-dex2oat / \
    --nice-name="termux-x11 com.termux.x11 :1 -ac" \
    com.termux.x11.Loader :1 -ac
