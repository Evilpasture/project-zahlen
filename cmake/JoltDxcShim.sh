#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# cmake/JoltDxcShim.sh
#
# Stand-in for dxc that lets Jolt cook its Vulkan compute shaders on a machine
# where dxc does not exist.
#
# Jolt compiles Jolt/Shaders/*.hlsl to SPIR-V with dxc and never searches for
# it: Jolt/Jolt.cmake takes the glslc path find_package(Vulkan) produced and
# renames the executable,
#     string(REPLACE "glslc" "dxc" Vulkan_dxc_EXECUTABLE ${Vulkan_GLSLC_EXECUTABLE})
# so with no glslc the variable stays Vulkan_GLSLC_EXECUTABLE-NOTFOUND, the
# rename changes nothing, and every shader rule execs a program with that name
# (exit 127). That is unrecoverable on targets no dxc is released for: Fedora
# ships no directx-shader-compiler for aarch64, conda-forge has no linux-aarch64
# build, the Vulkan SDK's arm64 packages omit it, and the upstream DXC release
# is x86_64-only.
#
# The Slang this repository already vendors compiles the same HLSL to the same
# SPIR-V, so this shim accepts dxc's command line and runs slangc with the
# equivalent flags. Equivalence is verified, not assumed: with
# -fvk-use-dx-layout and no -matrix-layout override, Slang reproduces dxc's
# buffer layout (12-byte float3, 16-byte-strided float4x4 columns, identical
# member offsets) and binds descriptors in declaration order. Every Jolt shader
# cooked here was diffed against a real dxc build: descriptor count, binding
# numbers, Block/BufferBlock decorations, all member offsets and array strides,
# and the workgroup size match.
#
# Flag mapping (the complete set Jolt/Jolt.cmake passes):
#   -E <entry>                     -> -entry <entry>
#   -T <profile>                   -> -stage <stage>   (cs_6_* -> compute)
#   -Fo <file>                     -> -o <file>
#   -WX                            -> -warnings-as-errors all,
#                                     except 30081 (see below)
#   -Od                            -> -O0
#   -O0 .. -O3                     -> passed through
#   -I <dir>                       -> passed through
#   -fvk-use-dx-layout             -> passed through
#   -spirv                         -> -target spirv -profile spirv_1_3
#   -all_resources_bound, -Zi,
#   -Qembed_debug, -Qstrip_debug,
#   -fspv-entrypoint-name=<name>   -> dropped (Vulkan needs none of them)
#   -fspv-debug=<fmt>              -> -g3
# Any other flag is reported and dropped; unreadable arguments are an error.
#
# The profile is spirv_1_3 rather than dxc's 1.0 default because Slang warns
# E50011 ("SPIR-V version too old") below 1.3, and Jolt compiles with -WX, which
# the mapping above turns into an error. 1.3 emits the same decorations and
# layouts verified above and asks for no capability a compute pipeline would not
# already need.
#
# One Slang diagnostic is exempt from -WX: 30081, "implicit conversion from
# 'uint' to 'bool' is not recommended", which HairIntegrate.h triggers with
# `if (!inMaterial.mEnableGrid)`. That is legal HLSL and dxc compiles it without
# a word; only Slang 2026.20's style check objects, so failing the cook over it
# would reject shader source dxc accepts. Every other diagnostic stays an error.
#
# slangc is located in this order: $ZHLN_JOLT_SLANGC, $SLANG_BIN/slangc,
# $VULKAN_SDK/bin/slangc, PATH, then the newest
# build/*/extern/slang/*/bin/slangc of the source tree this script lives in --
# the vendored Slang target. ZHLN_JOLT_SLANGC pins a specific compiler.

set -euo pipefail

die() {
    printf 'JoltDxcShim: %s\n' "$*" >&2
    exit 1
}

note() {
    printf 'JoltDxcShim: %s\n' "$*" >&2
}

find_slangc() {
    if [ -n "${ZHLN_JOLT_SLANGC:-}" ]; then
        [ -x "${ZHLN_JOLT_SLANGC}" ] || die "ZHLN_JOLT_SLANGC=${ZHLN_JOLT_SLANGC} is not executable"
        printf '%s\n' "${ZHLN_JOLT_SLANGC}"
        return 0
    fi
    local dir
    for dir in "${SLANG_BIN:-}" "${VULKAN_SDK:+${VULKAN_SDK}/bin}"; do
        if [ -n "${dir}" ] && [ -x "${dir}/slangc" ]; then
            printf '%s\n' "${dir}/slangc"
            return 0
        fi
    done
    if command -v slangc >/dev/null 2>&1; then
        command -v slangc
        return 0
    fi
    # The vendored tree: <repo>/build/<preset>/extern/slang/<config>/bin/slangc.
    local root newest
    root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
    newest=$(ls -t "${root}"/build/*/extern/slang/*/bin/slangc 2>/dev/null | head -n 1 || true)
    if [ -n "${newest}" ] && [ -x "${newest}" ]; then
        printf '%s\n' "${newest}"
        return 0
    fi
    return 1
}

entry=""
stage=""
output=""
profile="${ZHLN_JOLT_SLANG_PROFILE:-spirv_1_3}"
sources=()
slang_args=()

map_profile() {
    case "$1" in
        cs_*) stage=compute ;;
        ps_*) stage=fragment ;;
        vs_*) stage=vertex ;;
        gs_*) stage=geometry ;;
        hs_*) stage=hull ;;
        ds_*) stage=domain ;;
        ms_*) stage=mesh ;;
        as_*) stage=amplification ;;
        *)    die "unsupported shader profile '$1'" ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        -E)  [ $# -ge 2 ] || die "-E needs an entry point"; entry=$2; shift 2 ;;
        -E*) entry=${1#-E}; shift ;;
        -T)  [ $# -ge 2 ] || die "-T needs a profile"; map_profile "$2"; shift 2 ;;
        -T*) map_profile "${1#-T}"; shift ;;
        -Fo) [ $# -ge 2 ] || die "-Fo needs a path"; output=$2; shift 2 ;;
        -Fo*) output=${1#-Fo}; shift ;;
        -I)  [ $# -ge 2 ] || die "-I needs a path"; slang_args+=(-I "$2"); shift 2 ;;
        -I*) slang_args+=("$1"); shift ;;
        # -warnings-disable last: it wins over -warnings-as-errors all.
        -WX) slang_args+=(-warnings-as-errors all -warnings-disable 30081); shift ;;
        -Od) slang_args+=(-O0); shift ;;
        -O0|-O1|-O2|-O3) slang_args+=("$1"); shift ;;
        -fspv-debug=*) slang_args+=(-g3); shift ;;
        -spirv|-all_resources_bound|-Zi|-Qembed_debug|-Qstrip_debug|-Qstrip_reflect) shift ;;
        -fspv-entrypoint-name=*|-fspv-target-env=*|-fvk-binding-shift=*) shift ;;
        -fvk-use-dx-layout) slang_args+=("$1"); shift ;;
        -*) note "dropping unsupported flag $1"; shift ;;
        *)  sources+=("$1"); shift ;;
    esac
done

[ "${#sources[@]}" -gt 0 ] || die "no shader source given"
[ "${#sources[@]}" -eq 1 ] || die "expected one shader source, got ${#sources[@]}"
[ -n "${output}" ] || die "no output file given (-Fo)"
[ -n "${stage}" ] || die "no shader stage given (-T)"

slangc=$(find_slangc) || die "slangc not found; set ZHLN_JOLT_SLANGC or build the vendored Slang target"

exec "${slangc}" \
    -entry "${entry:-main}" \
    -stage "${stage}" \
    -target spirv \
    -profile "${profile}" \
    "${slang_args[@]}" \
    -o "${output}" \
    "${sources[0]}"
