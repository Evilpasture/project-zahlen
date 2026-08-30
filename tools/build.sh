#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Exit immediately if any command fails
set -e

BASE_BUILD_DIR="build"

SPECIFIED_COMPILER=""
COMPILER_CC=""
COMPILER_CXX=""
CMAKE_CONF_ARGS=()
BUILD_FLAGS=()

# 1. Parse arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --clang) 
            SPECIFIED_COMPILER="clang"
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
            SPECIFIED_COMPILER="gcc"
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
        --p2996)
            SPECIFIED_COMPILER="p2996"
            # Find the root directory (either clang-p2996 or llvm-p2996)
            P2996_ROOT=""
            for dir in "$HOME/clang-p2996" "$HOME/llvm-p2996" "../clang-p2996" "../llvm-p2996"; do
                if [[ -d "$dir" ]]; then
                    P2996_ROOT="$(cd "$dir" && pwd)"
                    break
                fi
            done

            if [[ -z "$P2996_ROOT" ]]; then
                echo "Error: Could not find custom p2996 directory (~/llvm-p2996 or ~/clang-p2996)." >&2
                exit 1
            fi

            # Determine install vs build-tree paths
            if [[ -d "$P2996_ROOT/install" ]]; then
                P2996_INSTALL="$P2996_ROOT/install"
                P2996_BUILD="$P2996_ROOT/build"
            elif [[ -x "$P2996_ROOT/build/bin/clang" ]]; then
                # Handle in-tree build directories on Linux
                P2996_INSTALL="$P2996_ROOT/build"
                P2996_BUILD="$P2996_ROOT/build"
            else
                echo "Error: Found $P2996_ROOT, but could not find compiled binaries in install/ or build/bin/." >&2
                exit 1
            fi

            COMPILER_CC="$P2996_INSTALL/bin/clang"
            COMPILER_CXX="$P2996_INSTALL/bin/clang++"

            # Dynamic library loader flag: -Wl,-rpath on Linux vs -Wl,-rpath on macOS
            LIB_DIR="$P2996_INSTALL/lib"
            [[ -d "$P2996_INSTALL/lib64" ]] && LIB_DIR="$P2996_INSTALL/lib64"

            CMAKE_CONF_ARGS+=(
                "-DCMAKE_PREFIX_PATH=$P2996_INSTALL"
                "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -Wno-unused-command-line-argument -L$LIB_DIR"
                "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath,$LIB_DIR -L$LIB_DIR"
                "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-rpath,$LIB_DIR -L$LIB_DIR"
                "-DZHLN_USE_CUSTOM_LIBCXX=ON"
                "-DLLVM_BLOOMBERG_ROOT=$P2996_ROOT"
                "-DLLVM_BLOOMBERG_BUILD=$P2996_BUILD"
            )
            ;;
        # Allow passing -D flags directly to CMake configuration if needed
        -D*)
            CMAKE_CONF_ARGS+=("$1")
            ;;
        # Anything else is passed to cmake --build (e.g., target name, --clean-first)
        *) 
            BUILD_FLAGS+=("$1") 
            ;;
    esac
    shift
done

# 2. Handle directory setup & auto-migration of legacy flat builds
mkdir -p "$BASE_BUILD_DIR"

if [[ -f "$BASE_BUILD_DIR/CMakeCache.txt" ]]; then
    echo "--- Detected legacy flat build layout: Migrating assets to build/shared_assets/ ---"
    mkdir -p "$BASE_BUILD_DIR/shared_assets"
    
    # Move existing assets if they are actual directories and not symlinks
    if [[ -d "$BASE_BUILD_DIR/build_assets" && ! -L "$BASE_BUILD_DIR/build_assets" ]]; then
        mv "$BASE_BUILD_DIR/build_assets" "$BASE_BUILD_DIR/shared_assets/"
    fi
    if [[ -d "$BASE_BUILD_DIR/data" && ! -L "$BASE_BUILD_DIR/data" ]]; then
        mv "$BASE_BUILD_DIR/data" "$BASE_BUILD_DIR/shared_assets/"
    fi
    
    # Remove old root build debris
    rm -rf "$BASE_BUILD_DIR/CMakeCache.txt" "$BASE_BUILD_DIR/CMakeFiles" "$BASE_BUILD_DIR"/*.ninja "$BASE_BUILD_DIR"/transpiled
fi

# Ensure shared asset directory exists
mkdir -p "$BASE_BUILD_DIR/shared_assets"

# 3. Determine active build directory (Resolve "Last Used" or new switch)
if [[ -n "$SPECIFIED_COMPILER" ]]; then
    COMPILER_TAG="$SPECIFIED_COMPILER"
elif [[ -L "$BASE_BUILD_DIR/current" ]]; then
    COMPILER_TAG="$(basename "$(readlink "$BASE_BUILD_DIR/current")")"
else
    COMPILER_TAG="default"
fi

BUILD_DIR="$BASE_BUILD_DIR/$COMPILER_TAG"
LOG_FILE="$BUILD_DIR/build.log"

mkdir -p "$BUILD_DIR"

# Update 'build/current' to point to the active build folder
(cd "$BASE_BUILD_DIR" && ln -sfn "$COMPILER_TAG" current)

# 4. Configuration (Only runs if unconfigured or new -D flags are provided)
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] || [ ${#CMAKE_CONF_ARGS[@]} -gt 0 ]; then
    echo "--- Configuring [$COMPILER_TAG] in '$BUILD_DIR' ---"
    
    if [[ -n "$COMPILER_CC" ]]; then
        export CC="$COMPILER_CC"
        export CXX="$COMPILER_CXX"
    fi
    
    cmake -GNinja -B"$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "${CMAKE_CONF_ARGS[@]}"
fi

# Always symlink compile_commands.json from the active build to the project root
if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
    ln -sf "$BUILD_DIR/compile_commands.json" ./compile_commands.json
fi

# 5. Detect available CPU cores across platforms
if command -v nproc &> /dev/null; then
    NPROCS=$(nproc)                             # Linux standard
elif command -v sysctl &> /dev/null && sysctl -n hw.ncpu &> /dev/null; then
    NPROCS=$(sysctl -n hw.ncpu)                 # macOS standard
else
    NPROCS=2                                    # Safe fallback
fi

# 6. Build and log
echo "--- Starting build [$COMPILER_TAG] (Active: $BUILD_DIR)... ---"
cmake --build "$BUILD_DIR" --parallel "$NPROCS" "${BUILD_FLAGS[@]}" 2>&1 | tee "$LOG_FILE"

# 7. Handle the result
BUILD_STATUS=${PIPESTATUS[0]}

if [ $BUILD_STATUS -eq 0 ]; then
    echo "--- Build [$COMPILER_TAG] successful! ---"
else
    echo "--- Build [$COMPILER_TAG] FAILED. ---"
    tail -n 10 "$LOG_FILE"
    exit $BUILD_STATUS
fi
