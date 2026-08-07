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
        --clang) 
            # Check for macOS Homebrew paths first, fallback to standard system clang
            if [[ -x "/opt/homebrew/opt/llvm/bin/clang++" ]]; then
                COMPILER_CC="/opt/homebrew/opt/llvm/bin/clang"
                COMPILER_CXX="/opt/homebrew/opt/llvm/bin/clang++"
            elif [[ -x "/usr/local/opt/llvm/bin/clang++" ]]; then
                COMPILER_CC="/usr/local/opt/llvm/bin/clang"
                COMPILER_CXX="/usr/local/opt/llvm/bin/clang++"
            else
                COMPILER_CC="clang"
                COMPILER_CXX="clang++"
            fi
            ;;
        --gcc)
            # On macOS, check for Homebrew GCC 16 binaries first; fallback to system gcc/g++
            if [[ "$OSTYPE" == "darwin"* ]]; then
                if [[ -x "/opt/homebrew/bin/g++-16" ]]; then
                    COMPILER_CC="/opt/homebrew/bin/gcc-16"
                    COMPILER_CXX="/opt/homebrew/bin/g++-16"
                elif [[ -x "/usr/local/bin/g++-16" ]]; then
                    COMPILER_CC="/usr/local/bin/gcc-16"
                    COMPILER_CXX="/usr/local/bin/g++-16"
                elif command -v gcc-16 &> /dev/null; then
                    COMPILER_CC="gcc-16"
                    COMPILER_CXX="g++-16"
                else
                    COMPILER_CC="gcc"
                    COMPILER_CXX="g++"
                fi
            else
                # Linux and other Unix-like systems retain default behavior
                COMPILER_CC="gcc"
                COMPILER_CXX="g++"
            fi
            ;;
        # Anything else is treated as a build flag
        *) BUILD_FLAGS+=("$1") ;;
    esac
    shift
done

# 2. Configuration (Only runs if needed)
if [[ -n "$COMPILER_CC" && -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "--- Compiler switch requested: Clearing existing CMake cache ---"
    rm -f "$BUILD_DIR/CMakeCache.txt"
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "--- Configuring ---"
    # Only set CC/CXX env vars if they were explicitly requested via arguments
    # Otherwise, let CMake automatically detect the system default compiler
    if [[ -n "$COMPILER_CC" ]]; then
        export CC="$COMPILER_CC"
        export CXX="$COMPILER_CXX"
    fi
    
    cmake -GNinja -B"$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-g"
fi

# 3. Detect available CPU cores across platforms
if command -v nproc &> /dev/null; then
    NPROCS=$(nproc)                             # Linux standard
elif command -v sysctl &> /dev/null && sysctl -n hw.ncpu &> /dev/null; then
    NPROCS=$(sysctl -n hw.ncpu)                 # macOS standard
else
    NPROCS=2                                    # Safe fallback
fi

# 4. Build and log
echo "--- Starting build... ---"
cmake --build "$BUILD_DIR" --parallel "$NPROCS" "${BUILD_FLAGS[@]}" 2>&1 | tee "$LOG_FILE"

# 5. Handle the result
BUILD_STATUS=${PIPESTATUS[0]}

if [ $BUILD_STATUS -eq 0 ]; then
    echo "--- Build successful! ---"
else
    echo "--- Build FAILED. ---"
    tail -n 10 "$LOG_FILE"
    exit $BUILD_STATUS
fi
