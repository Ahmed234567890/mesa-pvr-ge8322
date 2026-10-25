cmake_minimum_required(VERSION 3.16)
project(sysvk C)

find_package(Threads REQUIRED)

set(ANDROID_HEADERS_DIR "" CACHE PATH "Android platform headers (hardware/hwvulkan.h)")
set(HYBRIS_LIBRARY_DIR "" CACHE PATH "Directory containing libhardware.so")
set(HYBRIS_COMMON_LIBRARY_DIR "" CACHE PATH "Directory containing libhybris-common.so")

add_library(sysvk SHARED sysvk.c)
target_include_directories(sysvk PRIVATE "${ANDROID_HEADERS_DIR}")

find_library(HARDWARE_LIBRARY
    NAMES hardware
    HINTS "${HYBRIS_LIBRARY_DIR}"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT HARDWARE_LIBRARY)
    message(FATAL_ERROR "libhardware not found; set HYBRIS_LIBRARY_DIR")
endif()
target_link_libraries(sysvk PRIVATE "${HARDWARE_LIBRARY}" Threads::Threads)
if(HYBRIS_COMMON_LIBRARY_DIR)
    target_link_options(sysvk PRIVATE "-Wl,-rpath-link,${HYBRIS_COMMON_LIBRARY_DIR}")
endif()

install(TARGETS sysvk LIBRARY DESTINATION lib)
install(FILES sysvk_icd.json DESTINATION share/vulkan/icd.d/)
