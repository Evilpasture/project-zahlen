// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the engine hands the renderer once a frame: the scene state it authored.
//
// Two structs, one boundary. `FrameData` carries the view, the sun, the sky, the
// probe and the jitter -- plus the settings the frame was authored with. `LightDesc`
// carries one light of the scene. Both are Jolt's compute types in named fields: no
// padding a shader needs, no lane sharing, no field the renderer owns. The clock is
// `frameClock`, not `camPos.w`; intensity is `sunIntensity`, not `lightDir.w`; a flag
// is a flag.
//
// The shader's own structs (FrameUniforms, Light, the particle structs) are generated
// from the Slang module by tools/zshader and live only behind src/render, which packs
// these into them at the submit boundary (src/render/GpuPack.cpp). Nothing under
// include/ names a generated type, which is what lets the engine build with no shader
// tool having run: the engine is a producer of scene terms, not a consumer of the ABI.

#include <Zahlen/Render/GpuEnums.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <array>
#include <cstdint>

namespace ZHLN {

struct FrameData {
    JPH::Mat44 viewProj {};
    JPH::Mat44 unjitteredViewProj {};
    JPH::Mat44 prevUnjitteredViewProj {};
    JPH::Mat44 invViewProj {};

    JPH::Vec3 camPosition {};
    float     frameClock = 0.0f; // seconds, wrapped: the clock shaders animate from

    JPH::Vec3 sunDirection {0.0f, -1.0f, 0.0f};
    float     sunIntensity = 0.0f;
    JPH::Vec3 sunRadiance {};

    // The IBL probe the frame was authored with, if any.
    JPH::Vec3 probeMin {};
    JPH::Vec3 probeMax {};
    JPH::Vec3 probePos {};

    JPH::Vec4 skyZenith {0.0f, 0.0f, 0.0f, 1.0f};
    JPH::Vec4 skyHorizon {0.0f, 0.0f, 0.0f, 1.0f};
    JPH::Vec4 skyGround {0.0f, 0.0f, 0.0f, 1.0f};

    // This frame's temporal jitter in x/y and the previous frame's in z/w.
    JPH::Vec4 jitter {};

    // The settings the frame was authored with; the same member types
    // GraphicsSettings holds, so nothing is converted on the way in.
    float    shadowWidth = 0.0f;
    uint32_t shadowResolution = 0;
    float    sunSize = 0.05f;
    float    ambientExposure = 1.0f;
    int32_t  useLocalProbe = 0;
    int32_t  enableRTR = 0;
    int32_t  fullBright = 0;
};

// One scene light. `shadowLayer` is the renderer's own state riding along: it is
// assigned when a light is admitted to a shadow cascade, and the engine keeps the
// assignment with the light so it survives to the next frame. Everything else is
// what the light *is*, in world space.
struct LightDesc {
    LightType type = LightType::Directional;
    JPH::Vec3 position {};
    JPH::Vec3 direction {};
    JPH::Vec3 color {};
    float     intensity = 0.0f;
    float     radius = 0.0f;
    float     range = 1000.0f;
    // An area light's quad: one column per lane of the shader's four points.
    JPH::Mat44 points = JPH::Mat44::sIdentity();
    int32_t    shadowLayer = -1;
    uint32_t   twoSided = 0;
};

} // namespace ZHLN
