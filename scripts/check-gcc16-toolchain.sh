#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The fast half of the GCC 16.x matrix. It asks the four questions the tree
# actually cares about, with the exact compiler a cell just installed:
#
#   1. does -freflection give us C++26 static reflection (std::meta / ^^)?
#   2. does it defer Jolt's incomplete-static-member instantiation?
#   3. can its assembler handle the large constant data GCC 16 emits?
#   4. can a program using std::format link AND run (the runtime, not just cc1plus)?
#
# Probe 2 is verbatim from cmake/ToolchainCapabilities.cmake; probe 3 is its
# .base64 table check. Keeping the gate standalone means a compiler cell does
# not need Slang, Vulkan headers, Jolt or any submodule to be wrong about its
# own version. The scheduled full build is what exercises those.

set -Eeuo pipefail
trap 'printf "error: exit %s at line %s\n  command: %s\n" "$?" "${LINENO}" "${BASH_COMMAND}" >&2' ERR

CC_BIN="${CC:-}"
CXX_BIN="${CXX:-}"
FLAGS=(-std=c++26 -freflection -O2)
NO_RUN=0
WORK=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --cc)      CC_BIN="$2"; shift 2 ;;
        --cxx)     CXX_BIN="$2"; shift 2 ;;
        --flags)   read -r -a FLAGS <<< "$2"; shift 2 ;;
        --no-run)  NO_RUN=1; shift ;;
        --work)    WORK="$2"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 64 ;;
    esac
done

[ -n "$CXX_BIN" ] || { echo "error: --cxx (or \$CXX) is required" >&2; exit 64; }

# install-gcc16.sh exports a native path in CC/CXX (what CMake needs on
# Windows) and a POSIX one in ZHLN_GCC16_*_POSIX (what an MSYS2 shell can
# execute). Prefer the latter here, and keep the toolchain's own bin/ first so
# a probe binary finds the compiler's libstdc++/libwinpthread DLLs even if the
# MSYS2 shell reordered PATH.
if [ -n "${ZHLN_GCC16_CXX_POSIX:-}" ]; then CXX_BIN="$ZHLN_GCC16_CXX_POSIX"; fi
if [ -n "${ZHLN_GCC16_CC_POSIX:-}" ]; then CC_BIN="$ZHLN_GCC16_CC_POSIX"; fi
if command -v cygpath >/dev/null 2>&1; then
    CXX_BIN=$(cygpath -u "$CXX_BIN")
    [ -n "$CC_BIN" ] && CC_BIN=$(cygpath -u "$CC_BIN")
fi
toolchain_bin="$(cd "$(dirname "$CXX_BIN")" && pwd)"
export PATH="${toolchain_bin}:${PATH}"

# A compiled probe needs the pinned C++ runtime at load time. The conda
# wrappers set LD_LIBRARY_PATH for cc1plus but emit binaries without an rpath,
# so a program from that compiler cannot find GLIBCXX_3.4.36 on a stock runner.
case "${ZHLN_GCC16_KIND:-}" in
    conda)    runtime_dir="${ZHLN_GCC16_PREFIX:-}/lib" ;;
    arch-pkg) runtime_dir="${ZHLN_GCC16_PREFIX:-}/usr/lib" ;;
    *)        runtime_dir="" ;;
esac
if [ -n "$runtime_dir" ] && [ -d "$runtime_dir" ]; then
    export LD_LIBRARY_PATH="${runtime_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

[ -n "$WORK" ] || WORK="$(mktemp -d "${TMPDIR:-/tmp}/zahlen-gcc16-probe.XXXXXX")"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

log()  { printf '  %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

cat > "${WORK}/reflection.cpp" <<'EOF'
#include <meta>
struct Probe { int x; double y; };
constexpr auto info = ^^Probe;
static_assert(std::meta::is_class_type(info));
constexpr auto members = std::define_static_array(
    std::meta::members_of(info, std::meta::access_context::current()));
static_assert(members.size() >= 1);
int main() { return 0; }
EOF

# extern/JoltPhysics declares this inside Vec3. A trunk snapshot of GCC 16
# instantiates StaticArray<Vec3, 1026> while Vec3 is incomplete and rejects it;
# every released 16.x defers it.
cat > "${WORK}/jolt.cpp" <<'EOF'
template <typename T, int N>
struct StaticArray {
protected:
    int mSize = 0;
    union { T mElements[N]; unsigned char mDummy; };
};
class alignas(16) Vec3 {
public:
    static const StaticArray<Vec3, 1026> sUnitSphere;
    float x, y, z, w;
};
int main() { return 0; }
EOF

# 600 distinct values from a fixed seed, read through a volatile index so the
# optimiser cannot fold the table away before the assembler sees it. .base64 is
# what GAS >= 2.41 understands and older binutils rejects.
{
    seed=7
    i=0
    while [ "$i" -lt 600 ]; do
        seed=$(( (seed * 1103515245 + 12345) & 0x7fffffff ))
        printf '\t0x%xULL,\n' "$seed"
        i=$((i + 1))
    done
} > "${WORK}/base64-values.inc"
cat > "${WORK}/base64.cpp" <<'EOF'
#include <cstdint>
namespace {
const std::uint64_t kTable[] = {
#include "base64-values.inc"
};
}
extern "C" unsigned long long zahlen_probe_at(int index) { return kTable[index]; }
int main() {
    volatile int index = 123;
    volatile unsigned long long sink = zahlen_probe_at(index);
    (void)sink;
    return 0; }
EOF

# std::format is the reason the release toolchain needs GLIBCXX_3.4.36. A
# compiler whose libstdc++ is present but too old compiles this and then fails
# to link or load -- the failure the release lane cares about.
cat > "${WORK}/runtime.cpp" <<'EOF'
#include <format>
#include <string>
int main() {
    std::string s = std::format("gcc16-{}", 42);
    return s == "gcc16-42" ? 0 : 1;
}
EOF

# Some cells pin a compiler with a known, documented capability gap (see
# .github/gcc16/compilers.json `expected_probe_failures`). A gap is reported as
# an annotation and does not fail the job; a NEW failure always does, and a gap
# that unexpectedly starts passing asks for the manifest to be updated. This is
# how the matrix can keep an older 16.x visible without pretending it is green.
announce() {
    # GitHub workflow commands when running in Actions, plain text otherwise.
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        printf '::%s::%s\n' "$1" "$2"
    else
        printf '  [%s] %s\n' "$1" "$2"
    fi
}

is_expected() {
    local want=$1 item
    for item in ${EXPECTED_FAILURES:-}; do
        [ "$item" = "$want" ] && return 0
    done
    return 1
}

run_probe() {
    local name=$1 src="${WORK}/$1.cpp" exe="${WORK}/$1" rc=0
    [ -f "$src" ] || die "missing probe source $src"
    step "$name"
    if ! "$CXX_BIN" "${FLAGS[@]}" -I"$WORK" "$src" -o "$exe"; then
        rc=1
    elif [ "$NO_RUN" -eq 0 ] && ! "$exe"; then
        rc=1
    fi

    if [ "$rc" -eq 0 ]; then
        if is_expected "$name"; then
            announce warning "$name PASSED but compilers.json lists it as a known failure -- update expected_probe_failures"
            UNEXPECTED_PASSES=$((UNEXPECTED_PASSES + 1))
        fi
        if [ "$NO_RUN" -eq 0 ]; then
            log "$name: compile + link + run OK"
        else
            log "$name: compile + link OK (run skipped)"
        fi
        return 0
    fi

    if is_expected "$name"; then
        announce warning "$name FAILED as documented for this compiler (see compilers.json): ${name} capability gap"
        log "$name: known capability gap, not a regression"
        KNOWN_GAPS=$((KNOWN_GAPS + 1))
        return 0
    fi
    die "$name: compile/link/run FAILED with $CXX_BIN ${FLAGS[*]} and is not listed in expected_probe_failures"
}

EXPECTED_FAILURES="${ZHLN_GCC16_EXPECTED_FAILURES:-}"
KNOWN_GAPS=0
UNEXPECTED_PASSES=0

step "compiler under test"
log "CC  = ${CC_BIN:-<unset>}"
log "CXX = $CXX_BIN"
log "version = $("$CXX_BIN" -dumpfullversion -dumpversion 2>/dev/null | head -1)"
log "flags = ${FLAGS[*]}"
log "known gaps = ${EXPECTED_FAILURES:-none}"

for probe in reflection jolt base64 runtime; do
    run_probe "$probe"
done

printf '\n== gate passed: %s (%s new failures, %s documented gaps, %s stale entries)\n' \
    "$("$CXX_BIN" -dumpfullversion -dumpversion 2>/dev/null | head -1)" \
    "0" "$KNOWN_GAPS" "$UNEXPECTED_PASSES"
