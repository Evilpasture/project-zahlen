#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Exit immediately on command or pipeline failures
set -e
set -o pipefail

BASE_BUILD_DIR="build"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SPECIFIED_COMPILER=""
ENABLE_SANITIZER=false
USER_CMAKE_ARGS=()
BUILD_FLAGS=()

# 1. Parse arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --clang) 
            SPECIFIED_COMPILER="clang"
            ;;
        --gcc)
            SPECIFIED_COMPILER="gcc"
            ;;
        --p2996)
            SPECIFIED_COMPILER="p2996"
            ;;
        --zig)
            SPECIFIED_COMPILER="zig"
            ;;
        --sanitize|--sanitizer|--asan)
            ENABLE_SANITIZER=true
            ;;
        -D*)
            USER_CMAKE_ARGS+=("$1")
            ;;
        *) 
            BUILD_FLAGS+=("$1") 
            ;;
    esac
    shift
done

# 2. Determine base compiler flavor
mkdir -p "$BASE_BUILD_DIR"

if [[ -n "$SPECIFIED_COMPILER" ]]; then
    BASE_COMPILER="$SPECIFIED_COMPILER"
elif [[ -L "$BASE_BUILD_DIR/current" ]]; then
    CURRENT_ACTIVE="$(basename "$(readlink "$BASE_BUILD_DIR/current")")"
    BASE_COMPILER="${CURRENT_ACTIVE%-sanitized}"
else
    BASE_COMPILER="default"
fi

# 3. Determine final build folder name (COMPILER_TAG)
INTERNAL_CMAKE_ARGS=()

if [[ "$ENABLE_SANITIZER" == "true" ]]; then
    COMPILER_TAG="${BASE_COMPILER}-sanitized"
    INTERNAL_CMAKE_ARGS+=("-DUSE_SANITIZERS=ON")
elif [[ -n "$SPECIFIED_COMPILER" ]]; then
    COMPILER_TAG="$BASE_COMPILER"
    INTERNAL_CMAKE_ARGS+=("-DUSE_SANITIZERS=OFF")
elif [[ -L "$BASE_BUILD_DIR/current" ]]; then
    COMPILER_TAG="$(basename "$(readlink "$BASE_BUILD_DIR/current")")"
else
    COMPILER_TAG="default"
fi

BUILD_DIR="$BASE_BUILD_DIR/$COMPILER_TAG"
LOG_FILE="$BUILD_DIR/build.log"

# 4. Resolve compiler binaries
COMPILER_CC=""
COMPILER_CXX=""

case "$BASE_COMPILER" in
    clang)
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
    gcc)
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
            COMPILER_CC="gcc"
            COMPILER_CXX="g++"
        fi
        ;;
    zig)
        # Zig's bundled clang, driven through `zig cc` / `zig c++`. It speaks
        # C++26 and carries its own libc++, so the toolchain is self-contained:
        # `pip install ziglang cmake ninja` is enough to get a full build.
        #
        # It has no P2996 reflection (-freflection), so the project takes its
        # documented non-reflection fallback: "Using generated script for source
        # code flattening". Everything that does not need compile-time
        # reflection builds as usual.
        #
        # Prefer a `zig` on PATH; otherwise fall back to the PyPI `ziglang`
        # package, which has no `zig` entry point of its own and is driven as
        # `python -m ziglang`.
        if command -v zig > /dev/null 2>&1; then
            ZIG_DRIVER="$(command -v zig)"
        else
            ZIG_DRIVER=""
            for candidate in "${VIRTUAL_ENV:-}/bin/python" "python3"; do
                zig_py="$(command -v "$candidate" 2>/dev/null)" || continue
                [[ -n "$zig_py" ]] || continue
                if "$zig_py" -m ziglang version > /dev/null 2>&1; then
                    ZIG_DRIVER="$zig_py -m ziglang"
                    break
                fi
            done
        fi

        if [[ -z "$ZIG_DRIVER" ]]; then
            echo "Error: no Zig found. Install it with 'pip install ziglang' or put 'zig' on PATH." >&2
            exit 1
        fi

        # CMake needs one executable per compiler, and `zig c++` is two words,
        # so generate thin wrappers into the build directory.
        mkdir -p "$BUILD_DIR"
        COMPILER_CC="$BUILD_DIR/zig-cc"
        COMPILER_CXX="$BUILD_DIR/zig-c++"
        printf '#!/bin/sh\nexec %s cc "$@"\n' "$ZIG_DRIVER" > "$COMPILER_CC"
        printf '#!/bin/sh\nexec %s c++ "$@"\n' "$ZIG_DRIVER" > "$COMPILER_CXX"
        chmod +x "$COMPILER_CC" "$COMPILER_CXX"

        # Zig's clang builds reproducibly by default and promotes -Wdate-time
        # to an error; mimalloc (options.c) and CommandLine.cpp expand
        # __DATE__/__TIME__ and cannot be made reproducible.
        INTERNAL_CMAKE_ARGS+=("-DCMAKE_C_FLAGS=-Wno-error=date-time")
        INTERNAL_CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS=-Wno-error=date-time")

        # Zig's clang ships no clang-scan-deps binary, so CMake's P1689 C++
        # module dependency scanning has no tool to run and every target fails
        # with CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS-NOTFOUND. This hook is pulled
        # in by project() and turns scanning back off (the root CMakeLists sets
        # CMAKE_CXX_SCAN_FOR_MODULES ON unconditionally, so -D cannot win).
        SCAN_HOOK="$BUILD_DIR/DisableModuleScanning.cmake"
        printf '# Generated by tools/build.sh --zig: Zig clang has no clang-scan-deps.\nset(CMAKE_CXX_SCAN_FOR_MODULES OFF)\n' > "$SCAN_HOOK"
        INTERNAL_CMAKE_ARGS+=("-DCMAKE_PROJECT_zahlen_INCLUDE=$SCAN_HOOK")

        # Slang is normally a GitHub release binary. If it is not installed,
        # fall back to the compiler that ships inside the PyPI `slangpy`
        # wheel -- see tools/slangc_slangpy.py. `pip install slangpy` then
        # completes an all-PyPI toolchain.
        if ! command -v slangc > /dev/null 2>&1; then
            slangc_py=""
            for candidate in "${VIRTUAL_ENV:-}/bin/python" "python3"; do
                slangc_py="$(command -v "$candidate" 2>/dev/null)" || continue
                [[ -n "$slangc_py" ]] || continue
                if "$slangc_py" -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('slangpy') else 1)" > /dev/null 2>&1; then
                    break
                fi
                slangc_py=""
            done

            if [[ -n "$slangc_py" ]]; then
                SLANG_SHIM="$BUILD_DIR/slangc"
                printf '#!/bin/sh\nexec %s "%s" "$@"\n' "$slangc_py" "${SCRIPT_DIR}/slangc_slangpy.py" > "$SLANG_SHIM"
                chmod +x "$SLANG_SHIM"
                INTERNAL_CMAKE_ARGS+=("-DSLANG_EXECUTABLE=$SLANG_SHIM")
            fi
        fi
        ;;
    p2996)
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

        if [[ -d "$P2996_INSTALL" ]]; then
            P2996_INSTALL="$P2996_ROOT/install"
            P2996_BUILD="$P2996_ROOT/build"
        elif [[ -x "$P2996_ROOT/build/bin/clang" ]]; then
            P2996_INSTALL="$P2996_ROOT/build"
            P2996_BUILD="$P2996_ROOT/build"
        else
            echo "Error: Found $P2996_ROOT, but could not find compiled binaries in install/ or build/bin/." >&2
            exit 1
        fi

        COMPILER_CC="$P2996_INSTALL/bin/clang"
        COMPILER_CXX="$P2996_INSTALL/bin/clang++"

        LIB_DIR="$P2996_INSTALL/lib"
        [[ -d "$P2996_INSTALL/lib64" ]] && LIB_DIR="$P2996_INSTALL/lib64"

        INTERNAL_CMAKE_ARGS+=(
            "-DCMAKE_PREFIX_PATH=$P2996_INSTALL"
            "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -Wno-unused-command-line-argument -L$LIB_DIR"
            "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath,$LIB_DIR -L$LIB_DIR"
            "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-rpath,$LIB_DIR -L$LIB_DIR"
            "-DZHLN_USE_CUSTOM_LIBCXX=ON"
            "-DLLVM_BLOOMBERG_ROOT=$P2996_ROOT"
            "-DLLVM_BLOOMBERG_BUILD=$P2996_BUILD"
        )
        ;;
esac

# 5. Directory setup & auto-migration
if [[ -f "$BASE_BUILD_DIR/CMakeCache.txt" ]]; then
    echo "--- Detected legacy flat build layout: Migrating assets to build/shared_assets/ ---"
    mkdir -p "$BASE_BUILD_DIR/shared_assets"
    
    if [[ -d "$BASE_BUILD_DIR/build_assets" && ! -L "$BASE_BUILD_DIR/build_assets" ]]; then
        mv "$BASE_BUILD_DIR/build_assets" "$BASE_BUILD_DIR/shared_assets/"
    fi
    if [[ -d "$BASE_BUILD_DIR/data" && ! -L "$BASE_BUILD_DIR/data" ]]; then
        mv "$BASE_BUILD_DIR/data" "$BASE_BUILD_DIR/shared_assets/"
    fi
    
    rm -rf "$BASE_BUILD_DIR/CMakeCache.txt" "$BASE_BUILD_DIR/CMakeFiles" "$BASE_BUILD_DIR"/*.ninja "$BASE_BUILD_DIR"/transpiled
fi

mkdir -p "$BASE_BUILD_DIR/shared_assets"
mkdir -p "$BUILD_DIR"

# Update 'build/current' symlink and create a top-level 'build/build.log' pointer
(cd "$BASE_BUILD_DIR" && ln -sfn "$COMPILER_TAG" current)
ln -sf "$COMPILER_TAG/build.log" "$BASE_BUILD_DIR/build.log"

# Truncate/initialize log file for this run
: > "$LOG_FILE"

# 6. Configuration (Pipes output to log file)
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] || [ ${#USER_CMAKE_ARGS[@]} -gt 0 ]; then
    echo "--- Configuring [$COMPILER_TAG] in '$BUILD_DIR' ---" | tee -a "$LOG_FILE"
    
    if [[ -n "$COMPILER_CC" ]]; then
        export CC="$COMPILER_CC"
        export CXX="$COMPILER_CXX"
    fi
    
    cmake -GNinja -B"$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "${INTERNAL_CMAKE_ARGS[@]}" \
        "${USER_CMAKE_ARGS[@]}" 2>&1 | tee -a "$LOG_FILE"
fi

# Link compile_commands.json to repository root
if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
    ln -sf "$BUILD_DIR/compile_commands.json" ./compile_commands.json
fi

# 7. CPU Core Detection
if command -v nproc &> /dev/null; then
    NPROCS=$(nproc)
elif command -v sysctl &> /dev/null && sysctl -n hw.ncpu &> /dev/null; then
    NPROCS=$(sysctl -n hw.ncpu)
else
    NPROCS=2
fi

# 8. Build and append to log
echo "--- Starting build [$COMPILER_TAG] (Active: $BUILD_DIR)... ---" | tee -a "$LOG_FILE"
cmake --build "$BUILD_DIR" --parallel "$NPROCS" "${BUILD_FLAGS[@]}" 2>&1 | tee -a "$LOG_FILE"

echo "--- Build [$COMPILER_TAG] successful! ---" | tee -a "$LOG_FILE"
