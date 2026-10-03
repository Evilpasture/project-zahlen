// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the engine hands the renderer, written by hand.
//
// Nothing here is generated. The GPU-side structs in resources/shaders/*.slang
// describe a *layout*: std430 packing, padding members, C arrays. Those are the
// renderer's business and stay inside src/render, where they are converted from
// these descriptions (src/render/LayoutConvert.cpp). What the engine authors is
// a description -- a camera position, an emitter's gravity, a light's cone --
// with the natural C++ type for each field, so a change to a shader's layout
// cannot silently reshape a public type or drag a code generator into a header
// that a target without shader tooling has to include.
//
// The direction of the split: engine -> description -> renderer -> GPU layout.

#include <Zahlen/Render/GpuEnums.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float2.h> // particle sizes: Jolt's 2-float storage class
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <cstdint>

namespace ZHLN {

// resources/shaders/particles.slang :: ParticleEmitterParams
struct ParticleEmitterDesc {
    JPH::Vec3         gravity {0.0f, -9.81f, 0.0f};
    float             drag = 0.2f;
    JPH::Vec3         turbulence {JPH::Vec3::sZero()};
    float             turbulenceFreq = 0.1f;
    JPH::Vec3         spawnOrigin {JPH::Vec3::sZero()};
    float             spawnRadius = 0.0f;
    JPH::Vec3         spawnBoxExtent {10.0f, 10.0f, 10.0f};
    float             loopBoundary = 0.0f;
    JPH::Vec3         initVelMin {-1.0f, -1.0f, -1.0f};
    float             lifetimeMin = 1.0f;
    JPH::Vec3         initVelMax {1.0f, 1.0f, 1.0f};
    float             lifetimeMax = 3.0f;
    JPH::Vec4         startColor {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Vec4         endColor {1.0f, 1.0f, 1.0f, 0.0f};
    JPH::Float2       startSize {0.1f, 0.1f};
    JPH::Float2       endSize {0.0f, 0.0f};
    float             spinSpeed = 0.0f;
    uint32_t          textureIndex = 1;
    ParticleAlignment alignment = ParticleAlignment::CameraBillboard;
    uint32_t          blendMode = 0;
};

// resources/shaders/particles.slang :: MeshParticleEmitterParams
struct MeshParticleEmitterDesc {
    JPH::Vec3 gravity {0.0f, -9.81f, 0.0f};
    float     drag = 0.2f;
    JPH::Vec3 turbulence {JPH::Vec3::sZero()};
    float     turbulenceFreq = 0.1f;
    JPH::Vec3 spawnOrigin {JPH::Vec3::sZero()};
    float     spawnRadius = 0.0f;
    JPH::Vec3 spawnBoxExtent {10.0f, 10.0f, 10.0f};
    float     loopBoundary = 0.0f;
    JPH::Vec3 initVelMin {-5.0f, 0.0f, -5.0f};
    float     lifetimeMin = 1.0f;
    JPH::Vec3 initVelMax {5.0f, 10.0f, 5.0f};
    float     lifetimeMax = 3.0f;
    JPH::Vec3 rotVelMin {-3.14f, -3.14f, -3.14f};
    float     scaleMin = 0.1f;
    JPH::Vec3 rotVelMax {3.14f, 3.14f, 3.14f};
    float     scaleMax = 0.5f;
    JPH::Vec4 startColor {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Vec4 endColor {1.0f, 1.0f, 1.0f, 1.0f};
};

// resources/shaders/particles.slang :: Particle. The renderer's storage element
// is this struct converted (src/render/LayoutConvert.cpp), which is what lets a
// VFX extension author particles without naming a GPU layout: it fills these
// four lanes and hands the span to RenderContext::UploadParticles.
struct ParticleDesc {
    JPH::Vec4 position {JPH::Vec4::sZero()};
    JPH::Vec4 velocity {JPH::Vec4::sZero()};
    JPH::Vec4 color {JPH::Vec4::sOne()};
    // Emitter-specific: the shaders read life/maxLife/size out of here.
    JPH::Vec4 params {JPH::Vec4::sZero()};
};

// resources/shaders/uniforms.slang :: Light. `positionView` is the light in view
// space: the engine has the view matrix when it packs lights, and doing it here
// keeps the descriptor free of a matrix the renderer would otherwise re-derive.
//
// Defaults are the zero state of the GPU struct these replace (uniforms.slang
// declares no [CxxDefault] on Light): a field the engine does not set reads
// exactly as it did when the engine zero-initialised the generated struct.
struct LightDesc {
    LightType type = LightType::Directional; // enumerator 0, i.e. the old zero
    JPH::Vec3 color {JPH::Vec3::sZero()};
    float     intensity = 0.0f;
    float     radius = 0.0f;
    JPH::Vec3 direction {JPH::Vec3::sZero()};
    float     range = 0.0f;
    JPH::Mat44 points = JPH::Mat44::sZero();
    float     innerConeCos = 0.0f;
    float     outerConeCos = 0.0f;
    uint32_t  twoSided = 0;
    int32_t   shadowLayer = 0;
    JPH::Vec3 position {JPH::Vec3::sZero()};
    JPH::Vec3 positionView {JPH::Vec3::sZero()};
};

// resources/shaders/uniforms.slang :: FrameUniforms -- the fields the *engine*
// authors. Everything else in that struct (screen resolution, light count, the
// cascade matrices, the SH probe, the viewmodel matrix) is the renderer's own
// state and is filled in RenderSetup.cpp, with no public type involved.
struct FrameViewData {
    JPH::Mat44 viewProj = JPH::Mat44::sIdentity();
    JPH::Mat44 unjitteredViewProj = JPH::Mat44::sIdentity();
    JPH::Mat44 prevUnjitteredViewProj = JPH::Mat44::sIdentity();
    JPH::Mat44 invViewProj = JPH::Mat44::sIdentity();

    JPH::Vec3 cameraPosition {JPH::Vec3::sZero()};
    // Shader-facing clock in seconds: wraps, and jumps are expected.
    float frameClock = 0.0f;

    JPH::Vec3 sunDirection {0.0f, -1.0f, 0.0f};
    float     sunIntensity = 0.0f;
    JPH::Vec4 sunRadiance {JPH::Vec4::sZero()};
    float     sunSize = 0.0f;

    JPH::Vec4 probeMin {JPH::Vec4::sZero()};
    JPH::Vec4 probeMax {JPH::Vec4::sZero()};
    JPH::Vec4 probePos {JPH::Vec4::sZero()};
    // TAA jitter of this frame and the previous one.
    JPH::Vec4 jitterParams {JPH::Vec4::sZero()};

    int32_t  enableRTR = 0;
    int32_t  fullBright = 0;
    float    shadowWidth = 0.0f;
    uint32_t shadowResolution = 0;
    float    ambientExposure = 0.0f;

    JPH::Vec4 skyZenith {JPH::Vec4::sZero()};
    JPH::Vec4 skyHorizon {JPH::Vec4::sZero()};
    JPH::Vec4 skyGround {JPH::Vec4::sZero()};
};

} // namespace ZHLN
