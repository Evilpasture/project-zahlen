// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The renderer's hand-written GPU layouts.
//
// tools/zshader emits every GPU-visible struct from the Slang module -- except
// this one, which it never sees. GpuLayout.hpp says nothing there is
// hand-written; this header is the complement. No public header names this
// type: the volumetric fill (when it lands) packs the public component here at
// the submit boundary, so the engine, its extensions and its tests compile
// without the shader cook having run.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Mat44.h>
#include <cstdint>

namespace ZHLN {

// Storage, not compute: nothing does vector maths on these; they are handed to
// the volumetric pass as-is, so they are Float4. (The struct is allocated and
// never filled today -- RenderInit sizes a buffer for it -- but its spelling is
// what a fill would write through.)
struct alignas(16) GPUVolumetricVolume {
    JPH::Mat44  invTransform;
    JPH::Float4 extentsAndType;
    JPH::Float4 colorAndDensity;
    JPH::Float4 emissiveAndAniso;
};
static_assert(sizeof(GPUVolumetricVolume) == 112);

}
