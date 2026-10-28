#!/bin/sh
# Global GPU environment for the Kali desktop on the SM-A032F (PowerVR GE8322).
#
# Source it from a session script, or run a command through it:
#
#   . /opt/pvr-bridge/desktop/bin/gpu-env
#   /opt/pvr-bridge/desktop/bin/gpu-env glxinfo -B
#
# What it sets up
#   * EGL/GLES      libhybris wrappers around the vendor PowerVR driver
#   * Desktop GL    GL4ES (GL 2.1) on top of vendor GLES, ahead of Mesa
#   * X11 winsys    HYBRIS_EGLPLATFORM=x11 so libhybris wraps X11 windows
#
# No LD_PRELOAD is used. The glibc __ctype_b / bionic TLS_SLOT_OPENGL
# collision is repaired inside the staged libEGL itself, so Android binaries
# started from the desktop keep working.
#
# Variables
#   PVR_DESKTOP_GL=gl4es   (default) hardware desktop GL 2.1 via GL4ES
#   PVR_DESKTOP_GL=mesa    software desktop GL 4.5 via Mesa llvmpipe
#
# Everything is overridable, so a test can point at a staging directory.

GPU_BRIDGE=${GPU_BRIDGE:-/opt/pvr-bridge}
GPU_EGL_DIR=${GPU_EGL_DIR:-$GPU_BRIDGE/opengl/lib}
GPU_GL4ES_DIR=${GPU_GL4ES_DIR:-$GPU_BRIDGE/gl4es/lib}
GPU_OPENCL_DIR=${GPU_OPENCL_DIR:-$GPU_BRIDGE/opencl/lib}
GPU_LIB_DIR=${GPU_LIB_DIR:-$GPU_BRIDGE/lib}

# Desktop OpenGL selection. GL4ES has to precede Mesa's libGL.so.1.
if [ "${PVR_DESKTOP_GL:-gl4es}" = "gl4es" ]; then
    LD_LIBRARY_PATH="$GPU_GL4ES_DIR:$GPU_EGL_DIR:$GPU_OPENCL_DIR:$GPU_LIB_DIR:/usr/lib/arm-linux-gnueabihf"
else
    LD_LIBRARY_PATH="$GPU_EGL_DIR:$GPU_OPENCL_DIR:$GPU_LIB_DIR:/usr/lib/arm-linux-gnueabihf"
fi
if [ -n "${LD_LIBRARY_PATH_EXTRA:-}" ]; then
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$LD_LIBRARY_PATH_EXTRA"
fi
export LD_LIBRARY_PATH

# GL4ES loads the vendor EGL/GLES through libhybris explicitly; without these
# it falls back to Mesa's EGL and renders on llvmpipe.
LIBGL_EGL="$GPU_EGL_DIR/libEGL.so"
LIBGL_GLES="$GPU_EGL_DIR/libGLESv2.so"
LIBGL_DEEPBIND=1
LIBGL_NOBANNER=1
export LIBGL_EGL LIBGL_GLES LIBGL_DEEPBIND LIBGL_NOBANNER

# libhybris: use the X11 winsys, not the Android passthrough one.
HYBRIS_EGLPLATFORM=x11
HYBRIS_EGLPLATFORM_DIR=${HYBRIS_EGLPLATFORM_DIR:-$GPU_BRIDGE/opengl/eglplatform}
# SysV SHM can fail from the Kali chroot; libhybris' X11 path otherwise
# passes shmat's (void *)-1 to memcpy. XPutImage is the safe fallback.
HYBRIS_X11_DISABLE_SHM=1
HYBRIS_LINKER_DIR=${HYBRIS_LINKER_DIR:-$GPU_BRIDGE/libhybris/linker}
HYBRIS_ANDROID_SDK_VERSION=${HYBRIS_ANDROID_SDK_VERSION:-34}
HYBRIS_LD_LIBRARY_PATH=${HYBRIS_LD_LIBRARY_PATH:-/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib}
export HYBRIS_EGLPLATFORM HYBRIS_EGLPLATFORM_DIR HYBRIS_X11_DISABLE_SHM
export HYBRIS_LINKER_DIR HYBRIS_ANDROID_SDK_VERSION HYBRIS_LD_LIBRARY_PATH

ANDROID_ROOT=${ANDROID_ROOT:-/system}
ANDROID_DATA=${ANDROID_DATA:-/data}
export ANDROID_ROOT ANDROID_DATA

# Vulkan: the vendor HAL through the sysvk ICD plus the X11 WSI layer. The
# layer resolves libahb-wrapper itself, so no LD_PRELOAD is needed here either.
VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$GPU_BRIDGE/share/vulkan/icd.d/sysvk_icd.json}
VK_ADD_IMPLICIT_LAYER_PATH=${VK_ADD_IMPLICIT_LAYER_PATH:-$GPU_BRIDGE/share/vulkan/implicit_layer.d}
# Mesa's device-select and anti-lag layers can intercept vendor device
# selection and presentation.
VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:-VK_LAYER_MESA_device_select,VK_LAYER_MESA_anti_lag}
unset VK_INSTANCE_LAYERS VK_LAYER_PATH 2>/dev/null || true
export VK_ICD_FILENAMES VK_ADD_IMPLICIT_LAYER_PATH VK_LOADER_LAYERS_DISABLE

# Run a command through the environment when arguments are given. When the
# file is sourced, "$#" is the sourcing shell's argument count, so an
# interactive shell without arguments just gets the variables.
if [ "$#" -gt 0 ]; then
    exec "$@"
fi
