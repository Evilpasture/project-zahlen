#!/usr/bin/env bash

# Exit immediately if any command fails
set -e

BUILD_DIR="build"
LOG_FILE="build.log"

# Default compilers (empty uses system defaults)
COMPILER_CC=""
COMPILER_CXX=""

# 1. Parse arguments for --clang or --gcc
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --clang) COMPILER_CC="clang"; COMPILER_CXX="clang++" ;;
        --gcc)   COMPILER_CC="gcc";   COMPILER_CXX="g++" ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done

# 2. Check if the build directory exists and has a cache
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "--- Configuring with CC=$COMPILER_CC CXX=$COMPILER_CXX ---"
    
    # We pass the compiler to cmake via environment variables
    CC=$COMPILER_CC CXX=$COMPILER_CXX \
    cmake -GNinja -B"$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-g"
fi

# 3. Build and log
echo "--- Starting build... ---"
cmake --build "$BUILD_DIR" --parallel "$(nproc)" 2>&1 | tee "$LOG_FILE"

# 4. Handle the result
BUILD_STATUS=${PIPESTATUS[0]}

if [ $BUILD_STATUS -eq 0 ]; then
    echo "--- Build successful! ---"
else
    echo "--- Build FAILED. ---"
    tail -n 10 "$LOG_FILE"
    exit $BUILD_STATUS
fi
