#!/bin/sh
# Build GL4ES (desktop OpenGL 2.1 -> vendor GLES) for armhf with HYBRIS + GLX_STUBS.
#
# GL4ES is the desktop OpenGL path on this phone: Mesa's Zink is gated out on
# the vendor ICD (see zink-gate.md), so GL 2.1 is translated to the vendor GLES
# driver that libhybris exposes.
#
# The build needs the armhf toolchain, an armhf sysroot with X11 headers, and
# cmake.  GL4ES dlopen()s libEGL/libGLESv2 at run time, so it does not link
# against libhybris; the runtime paths come from desktop/gpu-env.sh
# (LIBGL_EGL, LIBGL_GLES).
#
# Usage:
#   ARMHF_TOOLCHAIN=/path/to/arm-none-linux-gnueabihf \
#   ARMHF_SYSROOT=/path/to/armhf-sysroot \
#   OUT_DIR="$HOME/mesa-pvr-build/gl4es-out" \
#   opengl/build-gl4es.sh
#
# Optional:
#   GL4ES_SRC=/existing/gl4es/checkout   reuse a checkout instead of cloning
#   GL4ES_REF=<commit>                   override the pinned upstream commit
#   JOBS=8                               parallel build jobs (default 4)
set -eu

: "${ARMHF_TOOLCHAIN:?Set ARMHF_TOOLCHAIN to the Arm GNU toolchain root}"
: "${ARMHF_SYSROOT:?Set ARMHF_SYSROOT to the armhf sysroot}"
: "${OUT_DIR:?Set OUT_DIR to a private build directory outside this repository}"

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PATCH="$REPO_ROOT/opengl/patches/gl4es-hybris-eglplatform.patch"

# The commit this stack was verified against (FWGS merge on top of ptitSeb).
GL4ES_REF=${GL4ES_REF:-81547d986798e876de8b434193920b606a72363f}
GL4ES_SRC=${GL4ES_SRC:-$OUT_DIR/gl4es}
JOBS=${JOBS:-4}

mkdir -p "$OUT_DIR"

if [ ! -d "$GL4ES_SRC/.git" ]; then
    echo "== cloning GL4ES =="
    git clone https://github.com/ptitSeb/gl4es "$GL4ES_SRC"
fi

echo "== checking out $GL4ES_REF =="
git -C "$GL4ES_SRC" fetch -q origin "$GL4ES_REF" 2>/dev/null || true
git -C "$GL4ES_SRC" checkout -q "$GL4ES_REF"

if git -C "$GL4ES_SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "== hybris EGL platform patch already applied =="
else
    echo "== applying hybris EGL platform patch =="
    git -C "$GL4ES_SRC" apply "$PATCH"
fi

TOOLCHAIN="$OUT_DIR/armhf-toolchain.cmake"
cat >"$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TC "$ARMHF_TOOLCHAIN")
set(SYSROOT "$ARMHF_SYSROOT")

set(CMAKE_C_COMPILER \${TC}/bin/arm-none-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER \${TC}/bin/arm-none-linux-gnueabihf-g++)
set(CMAKE_AR \${TC}/bin/arm-none-linux-gnueabihf-ar)
set(CMAKE_RANLIB \${TC}/bin/arm-none-linux-gnueabihf-ranlib)
set(CMAKE_STRIP \${TC}/bin/arm-none-linux-gnueabihf-strip)

set(CMAKE_SYSROOT \${SYSROOT})
set(CMAKE_FIND_ROOT_PATH \${SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "--sysroot=\${SYSROOT} -O2 -fPIC")
set(CMAKE_CXX_FLAGS_INIT "--sysroot=\${SYSROOT} -O2 -fPIC")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--sysroot=\${SYSROOT} -L\${SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,\${SYSROOT}/usr/lib/arm-linux-gnueabihf")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=\${SYSROOT} -L\${SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,\${SYSROOT}/usr/lib/arm-linux-gnueabihf")
EOF

echo "== configuring =="
cmake -S "$GL4ES_SRC" -B "$OUT_DIR/build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DHYBRIS=ON -DGLX_STUBS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo

echo "== building with $JOBS jobs =="
cmake --build "$OUT_DIR/build" -j"$JOBS"

# GL4ES always writes libGL.so.1 into <source>/lib, not the build directory.
mkdir -p "$OUT_DIR/lib"
cp "$GL4ES_SRC/lib/libGL.so.1" "$OUT_DIR/lib/libGL.so.1"
ln -sf libGL.so.1 "$OUT_DIR/lib/libGL.so"

file "$OUT_DIR/lib/libGL.so.1"
echo
echo "built: $OUT_DIR/lib/libGL.so.1"
echo "stage inside the Kali chroot as:"
echo "  /opt/pvr-bridge/gl4es/lib/libGL.so.1 (+ libGL.so symlink)"
echo "and run desktop applications through desktop/gpu-env.sh."
