# Cross-compile for 32-bit ARM hard-float.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=.../armhf-toolchain.cmake \
#         -DARMHF_TOOLCHAIN=/path/to/arm-gnu-toolchain \
#         -DARMHF_SYSROOT=/path/to/armhf-sysroot ...
#
# ARMHF_TOOLCHAIN must contain bin/arm-none-linux-gnueabihf-*.

if(NOT DEFINED ARMHF_TOOLCHAIN)
    if(DEFINED ENV{ARMHF_TOOLCHAIN})
        set(ARMHF_TOOLCHAIN "$ENV{ARMHF_TOOLCHAIN}")
    else()
        set(ARMHF_TOOLCHAIN "/opt/arm-gnu-toolchain-14.3")
    endif()
endif()

if(NOT DEFINED ARMHF_SYSROOT)
    if(DEFINED ENV{ARMHF_SYSROOT})
        set(ARMHF_SYSROOT "$ENV{ARMHF_SYSROOT}")
    else()
        set(ARMHF_SYSROOT "/opt/armhf-sysroot")
    endif()
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER "${ARMHF_TOOLCHAIN}/bin/arm-none-linux-gnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${ARMHF_TOOLCHAIN}/bin/arm-none-linux-gnueabihf-g++")
set(CMAKE_AR "${ARMHF_TOOLCHAIN}/bin/arm-none-linux-gnueabihf-ar")
set(CMAKE_RANLIB "${ARMHF_TOOLCHAIN}/bin/arm-none-linux-gnueabihf-ranlib")
set(CMAKE_STRIP "${ARMHF_TOOLCHAIN}/bin/arm-none-linux-gnueabihf-strip")

set(CMAKE_SYSROOT "${ARMHF_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${ARMHF_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "--sysroot=${ARMHF_SYSROOT} -O2 -fPIC")
set(CMAKE_CXX_FLAGS_INIT "--sysroot=${ARMHF_SYSROOT} -O2 -fPIC")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "--sysroot=${ARMHF_SYSROOT} -L${ARMHF_SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,${ARMHF_SYSROOT}/usr/lib/arm-linux-gnueabihf")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--sysroot=${ARMHF_SYSROOT} -L${ARMHF_SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,${ARMHF_SYSROOT}/usr/lib/arm-linux-gnueabihf")
