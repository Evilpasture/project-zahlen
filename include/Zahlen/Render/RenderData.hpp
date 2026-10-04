// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The GPU-visible structs, defined once, by hand.
//
// There is no conversion layer between what the engine authors and what the
// shaders read: these *are* the shader ABI structs. One storage type per shader
// concept, decided by the Slang type and nothing else:
//
//     float2    -> JPH::Float2     |  computation: JPH::Float2
//     float3    -> JPH::Float3     |  computation: JPH::Vec3 (SIMD)
//     float4    -> JPH::Float4     |  computation: JPH::Vec4 / JPH::Quat
//     float4x4  -> JPH::Mat44      |  computation: JPH::Mat44
//     T name[N] -> std::array<T, N>|  computation: std::span<T, N>
//
// Crossing between the two columns is Jolt's own business -- JPH::Vec3::StoreFloat3,
// JPH::Vec4::StoreFloat4, JPH::Quat::StoreFloat4 -- and never a hand-written
// field-by-field conversion. tools/zshader reads the same Slang sources and emits
// a static_assert per member (offsetof and sizeof, from the reflected layout)
// against these structs, so a shader-side change is a compile error naming the
// field rather than a silent reshape. Nothing here is generated, and nothing here
// includes a generated header.
//
// Member order, padding and the `_padN` members are the shader's layout: they are
// part of the ABI, not decoration.

#include <Zahlen/Render/GpuEnums.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float2.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Mat44.h>
#include <array>
#include <cstdint>

namespace ZHLN {

// resources/shaders/particles.slang :: Particle. Four lanes, shader order:
// position, velocity, colour, and the emitter's own params (life, maxLife, size
// live in there). mesh/simulation shaders read it as a structured-buffer element.
struct Particle {
    JPH::Float4 position {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4 velocity {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4 color {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4 params {0.0f, 0.0f, 0.0f, 0.0f};
};

// resources/shaders/particles.slang :: ParticleEmitterParams. The CPU-side
// emitter: the update pass reads a row of these and integrates the system.
struct ParticleEmitterParams {
    JPH::Float3      gravity {0.0f, -9.81f, 0.0f};
    float            drag = 0.2f;
    JPH::Float3      turbulence {0.0f, 0.0f, 0.0f};
    float            turbulenceFreq = 0.1f;
    JPH::Float3      spawnOrigin {0.0f, 0.0f, 0.0f};
    float            spawnRadius = 0.0f;
    JPH::Float3      spawnBoxExtent {10.0f, 10.0f, 10.0f};
    float            loopBoundary = 0.0f;
    JPH::Float3      initVelMin {-1.0f, -1.0f, -1.0f};
    float            lifetimeMin = 1.0f;
    JPH::Float3      initVelMax {1.0f, 1.0f, 1.0f};
    float            lifetimeMax = 3.0f;
    JPH::Float4      startColor {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4      endColor {1.0f, 1.0f, 1.0f, 0.0f};
    JPH::Float2      startSize {0.1f, 0.1f};
    JPH::Float2      endSize {0.0f, 0.0f};
    float            spinSpeed = 0.0f;
    uint32_t         textureIndex = 1;
    ParticleAlignment alignment = ParticleAlignment::CameraBillboard;
    uint32_t         blendMode = 0;
};

// resources/shaders/particles.slang :: MeshParticleEmitterParams
struct MeshParticleEmitterParams {
    JPH::Float3 gravity {0.0f, -9.81f, 0.0f};
    float       drag = 0.2f;
    JPH::Float3 turbulence {0.0f, 0.0f, 0.0f};
    float       turbulenceFreq = 0.1f;
    JPH::Float3 spawnOrigin {0.0f, 0.0f, 0.0f};
    float       spawnRadius = 0.0f;
    JPH::Float3 spawnBoxExtent {10.0f, 10.0f, 10.0f};
    float       loopBoundary = 0.0f;
    JPH::Float3 initVelMin {-5.0f, 0.0f, -5.0f};
    float       lifetimeMin = 1.0f;
    JPH::Float3 initVelMax {5.0f, 10.0f, 5.0f};
    float       lifetimeMax = 3.0f;
    JPH::Float3 rotVelMin {-3.14f, -3.14f, -3.14f};
    float       scaleMin = 0.1f;
    JPH::Float3 rotVelMax {3.14f, 3.14f, 3.14f};
    float       scaleMax = 0.5f;
    JPH::Float4 startColor {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4 endColor {1.0f, 1.0f, 1.0f, 1.0f};
};

// resources/shaders/uniforms.slang :: Light. `positionView` is the light in view
// space: the engine has the view matrix when it fills this, and doing it here
// keeps the descriptor free of a matrix the renderer would otherwise re-derive.
//
// This struct is the shape the shaders read, so `_pad0` is ABI: `positionView` is
// a 16-byte member and cannot straddle a 16-byte boundary, so the shader's layout
// leaves 12 bytes before it and the host struct must too.
struct Light {
    JPH::Float3              position {0.0f, 0.0f, 0.0f};
    LightType                type = LightType::Directional; // enumerator 0: the zero state
    JPH::Float3              color {0.0f, 0.0f, 0.0f};
    float                    intensity = 0.0f;
    JPH::Float3              direction {0.0f, 0.0f, 0.0f};
    float                    range = 0.0f;
    std::array<JPH::Float4, 4> points {}; // an area light's quad, one column per lane
    float                    radius = 0.0f;
    float                    innerConeCos = 0.0f;
    float                    outerConeCos = 0.0f;
    uint32_t                 twoSided = 0;
    int32_t                  shadowLayer = 0;
    uint8_t                  _pad0[12]; // 132..143, where the shader's layout lands positionView
    JPH::Float4              positionView {0.0f, 0.0f, 0.0f, 0.0f};
};

// resources/shaders/uniforms.slang :: FrameUniforms.
//
// Filled in two places by design. The engine authors the view, the sun, the sky,
// the probe and the jitter (RenderSystem); the renderer fills what it alone knows
// -- screen resolution, light count, cascade splits, the SH payload, the viewmodel
// matrix -- after it receives the struct (RenderSetup::SetFrameData). One struct,
// one type, no packing step: `camPos.w` is the frame clock and `lightDir.w` the
// sun intensity because that is what the shader reads out of those lanes.
struct FrameUniforms {
    JPH::Mat44                 viewProj = JPH::Mat44::sIdentity();
    JPH::Mat44                 unjitteredViewProj = JPH::Mat44::sIdentity();
    JPH::Mat44                 prevUnjitteredViewProj = JPH::Mat44::sIdentity();
    std::array<JPH::Mat44, 4>  lightSpaceMatrices {}; // renderer: cascade matrices
    JPH::Mat44                 invViewProj = JPH::Mat44::sIdentity();
    JPH::Float4                camPos {0.0f, 0.0f, 0.0f, 0.0f}; // xyz position, w frame clock
    JPH::Float4                lightDir {0.0f, -1.0f, 0.0f, 0.0f}; // xyz sun direction, w intensity
    JPH::Float4                sunRadiance {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t                   lightCount = 0; // renderer
    float                      ambientExposure = 0.0f;
    float                      shadowWidth = 0.0f;
    uint32_t                   shadowResolution = 0;
    std::array<JPH::Float4, 9> sh {}; // renderer: the IBL probe's SH payload
    JPH::Float4                probeMin {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4                probeMax {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4                probePos {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4                jitterParams {0.0f, 0.0f, 0.0f, 0.0f}; // this frame's and the previous one's
    int32_t                    enableRTR = 0;
    float                      sunSize = 0.0f;
    uint8_t                    _pad0[8]; // 792..799: cascadeSplits is a 16-byte member
    JPH::Float4                cascadeSplits {0.0f, 0.0f, 0.0f, 0.0f}; // renderer
    int32_t                    numCascades = 0; // renderer
    int32_t                    fullBright = 0;
    JPH::Float2                screenResolution {0.0f, 0.0f}; // renderer
    JPH::Float4                skyZenith {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4                skyHorizon {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4                skyGround {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Mat44                 viewmodelViewProj = JPH::Mat44::sIdentity(); // renderer
    JPH::Mat44                 invProj = JPH::Mat44::sIdentity(); // renderer
    float                      nearZ = 0.0f; // renderer: the camera's
    float                      farZ = 1.0f;
    int32_t                    environmentMode = 0; // renderer
};

} // namespace ZHLN
