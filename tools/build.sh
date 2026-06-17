#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later



# Exit immediately if any command fails
set -e

BUILD_DIR="build"
LOG_FILE="build.log"

COMPILER_CC=""
COMPILER_CXX=""
BUILD_FLAGS=()  # Array to store extra flags

# 1. Parse arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --clang) COMPILER_CC="clang"; COMPILER_CXX="clang++" ;;
        --gcc)   COMPILER_CC="gcc";   COMPILER_CXX="g++" ;;
        # Anything else is treated as a build flag
        *) BUILD_FLAGS+=("$1") ;;
    esac
    shift
done

# 2. Configuration (Only runs if needed)
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "--- Configuring ---"
    CC=$COMPILER_CC CXX=$COMPILER_CXX \
    cmake -GNinja -B"$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-g"
fi

# 3. Build and log
echo "--- Starting build... ---"
# Pass the collected flags here, expanding the array
cmake --build "$BUILD_DIR" --parallel "$(nproc)" "${BUILD_FLAGS[@]}" 2>&1 | tee "$LOG_FILE"

# 4. Handle the result
BUILD_STATUS=${PIPESTATUS[0]}

if [ $BUILD_STATUS -eq 0 ]; then
    echo "--- Build successful! ---"
else
    echo "--- Build FAILED. ---"
    tail -n 10 "$LOG_FILE"
    exit $BUILD_STATUS
fi
