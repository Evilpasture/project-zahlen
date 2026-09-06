#!/usr/bin/env bash
# Build + run the host-only GUI button harness.
#
# The harness links the REAL src/gui/GUIContext.cpp against the REAL
# extern/clay/clay.h (submodule commit pinned by the repo). No re-implementation.
#
# The project normally builds with CMake + clang + C++26 static reflection +
# Vulkan. This harness needs none of that: it only needs a C++ compiler that can
# parse include/Zahlen and a C++ standard library with <format>/<expected>, so it
# runs on a GPU-less box.
#
# Usage:
#   tests/gui_harness/build.sh            # build + run the scenario replay
#   tests/gui_harness/build.sh --probe    # build + run the layout/hit-region probe
#   CXX=clang++ tests/gui_harness/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/gui_harness"
mkdir -p "$OUT"

CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
    ZIG_BIN="$(python3 -c 'import os,ziglang;print(os.path.join(os.path.dirname(ziglang.__file__),"zig"))' 2>/dev/null || true)"
    if command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    elif [[ -n "$ZIG_BIN" && -x "$ZIG_BIN" ]]; then
        # `pip install ziglang` ships a full clang + libc++ (has <format>/<expected>).
        CXX="$ZIG_BIN c++"
    else
        CXX=g++
    fi
fi
echo "[gui_harness] compiler: $CXX"

STD="-std=c++26"
$CXX $STD -x c++ -E - </dev/null >/dev/null 2>&1 || STD="-std=c++2b"

INCLUDES=(
    -I"$ROOT/include"
    -I"$ROOT/src"
    -I"$ROOT/extern/clay"
    -I"$ROOT/extern/JoltPhysics"
)
# Wno-* only: the harness must not change what is compiled.
WARN="-w"

# Force-included into every TU. Restores Reflect::EnumCount<KeyCode>() (and with
# it the InputStateComponent key bitset width) on toolchains without C++26 static
# reflection. See the header for why this is load-bearing.
SHIM=(-include "$ROOT/tests/gui_harness/reflection_shim.hpp")

set -x
$CXX $STD $WARN "${SHIM[@]}" "${INCLUDES[@]}" \
    "$ROOT/tests/gui_harness/main.cpp" \
    "$ROOT/tests/gui_harness/harness_stubs.cpp" \
    "$ROOT/src/gui/GUIContext.cpp" \
    "$ROOT/src/gui/Text.cpp" \
    "$ROOT/src/ecs/ECS.cpp" \
    -o "$OUT/gui_harness" \
    -lc++ -lm
set +x

echo "[gui_harness] built -> $OUT/gui_harness"
echo "[gui_harness] clay.h md5: $(md5sum "$ROOT/extern/clay/clay.h" | cut -d' ' -f1) (submodule $(git -C "$ROOT" submodule status extern/clay 2>/dev/null | tr -d ' -+' || echo unknown))"
exec "$OUT/gui_harness" "$@"
