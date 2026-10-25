cmake_minimum_required(VERSION 3.10)
project(ahb-wrapper C)

set(HYBRIS_INCLUDE_DIR "/usr/include/hybris" CACHE PATH "Directory containing hybris/dlfcn/dlfcn.h")
set(HYBRIS_LIBRARY_DIR "" CACHE PATH "Directory containing libhybris-common.so")

add_library(ahb-wrapper SHARED ahb-wrapper.c)
# hybris/dlfcn/dlfcn.h includes <hybris/common/dlfcn.h>, so both the
# hybris/ directory and its parent are needed on the include path.
target_include_directories(ahb-wrapper PRIVATE
    "${HYBRIS_INCLUDE_DIR}"
    "${HYBRIS_INCLUDE_DIR}/..")

find_library(HYBRIS_COMMON_LIBRARY
    NAMES hybris-common
    HINTS "${HYBRIS_LIBRARY_DIR}"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT HYBRIS_COMMON_LIBRARY)
    message(FATAL_ERROR "libhybris-common not found; set HYBRIS_LIBRARY_DIR")
endif()
target_link_libraries(ahb-wrapper PRIVATE "${HYBRIS_COMMON_LIBRARY}")

set_target_properties(ahb-wrapper PROPERTIES
    VERSION 1.0.0
    SOVERSION 1)

install(TARGETS ahb-wrapper LIBRARY DESTINATION lib)
