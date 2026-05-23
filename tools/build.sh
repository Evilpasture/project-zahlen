#!/usr/bin/env bash

# Exit immediately if any command fails
set -e

BUILD_DIR="build"
LOG_FILE="build.log"

# 1. Check if the build directory exists and has a cache
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "--- Build directory not configured. Running cmake configuration... ---"
    cmake -GNinja -B"$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-g"
fi

# 2. Build and log
echo "--- Starting build... ---"
# We use PIPESTATUS to capture the exit code of cmake even when piping
cmake --build "$BUILD_DIR" --parallel "$(nproc)" 2>&1 | tee "$LOG_FILE"

# 3. Handle the result
# PIPESTATUS[0] is the exit code of the first command in the pipe (cmake)
BUILD_STATUS=${PIPESTATUS[0]}

if [ $BUILD_STATUS -eq 0 ]; then
    echo "--- Build successful! ---"
else
    echo "--- Build FAILED. See $LOG_FILE for details. ---"
    # Optionally: tail the last few lines to show the error immediately
    echo "--- Last 10 lines of error: ---"
    tail -n 10 "$LOG_FILE"
    exit $BUILD_STATUS
fi