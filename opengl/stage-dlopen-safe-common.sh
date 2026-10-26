#!/bin/sh
# Stage a private libhybris-common copy that glibc permits applications to dlopen.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 BUILT_LIBHYBRIS_COMMON.so OUTPUT_PATH" >&2
    exit 2
fi

source_library=$1
output_library=$2

if ! command -v patchelf >/dev/null 2>&1; then
    echo "patchelf is required to clear the executable-stack flag" >&2
    exit 1
fi

mkdir -p "$(dirname -- "$output_library")"
cp -- "$source_library" "$output_library"
patchelf --clear-execstack "$output_library"

stack_status=$(patchelf --print-execstack "$output_library")
case "$stack_status" in
    *"execstack: -"*) ;;
    *)
        echo "failed to clear executable-stack flag: $stack_status" >&2
        exit 1
        ;;
esac

printf 'Staged private copy: %s (%s)\n' "$output_library" "$stack_status"
