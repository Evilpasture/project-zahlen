#!/bin/sh
# CMake invokes a compiler launcher as:
#   launcher <configured-compiler> <compiler-arguments...>
# The selected Blocks-capable compiler is appended to the launcher property;
# discard it and CMake's configured compiler, then forward the remaining args.
set -eu

if [ "$#" -lt 2 ]; then
    echo "Blocks C compiler launcher: expected clang and the configured compiler" >&2
    exit 2
fi

blocks_compiler=$1
shift
shift
exec "$blocks_compiler" "$@"
