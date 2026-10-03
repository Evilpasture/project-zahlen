// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LayoutConvert.hpp"
#include <cstring>

// Vector members are C arrays in the generated structs (a plain float3 and a
// [CxxArray] float4 spell the same way; only a bare float4 becomes JPH::Vec4),
// so the vector fields copy lane-by-lane with the widths checked here rather
// than at every call site.

namespace ZHLN {

namespace {

// Three overloads rather than one template: JPH::Vec3 occupies 16 bytes (it is
// 16-byte aligned, though it has three lanes), so a size-only check would let a
// four-lane field be filled from a three-lane source -- or vice versa -- and read
// padding. A field paired with the wrong source type does not compile at all.
void Store(float (&dst)[2], const JPH::Float2& src) noexcept {
    dst[0] = src.x;
    dst[1] = src.y;
}

void Store(float (&dst)[3], JPH::Vec3Arg src) noexcept {
    dst[0] = src.GetX();
    dst[1] = src.GetY();
    dst[2] = src.GetZ();
}

void Store(float (&dst)[4], JPH::Vec4Arg src) noexcept {
    dst[0] = src.GetX();
    dst[1] = src.GetY();
    dst[2] = src.GetZ();
    dst[3] = src.GetW();
}

} // namespace

auto ToGpu(const ParticleDesc& desc) noexcept -> GeneratedGpu::Particle {
    // One lane per member, assigned rather than memcpy'd: the generated struct is
    // four JPH::Vec4 in shader order, and this is where a reordering would show.
    return GeneratedGpu::Particle {.position = desc.position, .velocity = desc.velocity, .color = desc.color, .params = desc.params};
}

auto ToGpu(const ParticleEmitterDesc& desc) noexcept -> GeneratedGpu::ParticleEmitterParams {
    GeneratedGpu::ParticleEmitterParams gpu {};
    Store(gpu.gravity, desc.gravity);
    gpu.drag = desc.drag;
    Store(gpu.turbulence, desc.turbulence);
    gpu.turbulenceFreq = desc.turbulenceFreq;
    Store(gpu.spawnOrigin, desc.spawnOrigin);
    gpu.spawnRadius = desc.spawnRadius;
    Store(gpu.spawnBoxExtent, desc.spawnBoxExtent);
    gpu.loopBoundary = desc.loopBoundary;
    Store(gpu.initVelMin, desc.initVelMin);
    gpu.lifetimeMin = desc.lifetimeMin;
    Store(gpu.initVelMax, desc.initVelMax);
    gpu.lifetimeMax = desc.lifetimeMax;
    Store(gpu.startColor, desc.startColor);
    Store(gpu.endColor, desc.endColor);
    Store(gpu.startSize, desc.startSize);
    Store(gpu.endSize, desc.endSize);
    gpu.spinSpeed    = desc.spinSpeed;
    gpu.textureIndex = desc.textureIndex;
    gpu.alignment    = desc.alignment;
    gpu.blendMode    = desc.blendMode;
    return gpu;
}

auto ToGpu(const MeshParticleEmitterDesc& desc) noexcept -> GeneratedGpu::MeshParticleEmitterParams {
    GeneratedGpu::MeshParticleEmitterParams gpu {};
    Store(gpu.gravity, desc.gravity);
    gpu.drag = desc.drag;
    Store(gpu.turbulence, desc.turbulence);
    gpu.turbulenceFreq = desc.turbulenceFreq;
    Store(gpu.spawnOrigin, desc.spawnOrigin);
    gpu.spawnRadius = desc.spawnRadius;
    Store(gpu.spawnBoxExtent, desc.spawnBoxExtent);
    gpu.loopBoundary = desc.loopBoundary;
    Store(gpu.initVelMin, desc.initVelMin);
    gpu.lifetimeMin = desc.lifetimeMin;
    Store(gpu.initVelMax, desc.initVelMax);
    gpu.lifetimeMax = desc.lifetimeMax;
    Store(gpu.rotVelMin, desc.rotVelMin);
    gpu.scaleMin = desc.scaleMin;
    Store(gpu.rotVelMax, desc.rotVelMax);
    gpu.scaleMax = desc.scaleMax;
    Store(gpu.startColor, desc.startColor);
    Store(gpu.endColor, desc.endColor);
    return gpu;
}

auto ToGpu(const LightDesc& desc) noexcept -> GeneratedGpu::Light {
    GeneratedGpu::Light gpu {};
    Store(gpu.position, desc.position);
    gpu.type      = desc.type;
    Store(gpu.color, desc.color);
    gpu.intensity = desc.intensity;
    Store(gpu.direction, desc.direction);
    gpu.range          = desc.range;
    // JPH::Mat44 is four column vectors of four floats: exactly the field.
    std::memcpy(&gpu.points[0][0], &desc.points, sizeof(JPH::Mat44));
    gpu.radius         = desc.radius;
    gpu.innerConeCos   = desc.innerConeCos;
    gpu.outerConeCos   = desc.outerConeCos;
    gpu.twoSided       = desc.twoSided;
    gpu.shadowLayer    = desc.shadowLayer;
    const JPH::Vec4 viewPos {desc.positionView, 0.0f};
    Store(gpu.positionView, viewPos);
    return gpu;
}

auto ToGpu(const FrameViewData& view) noexcept -> GeneratedGpu::FrameUniforms {
    GeneratedGpu::FrameUniforms gpu {};
    gpu.viewProj               = view.viewProj;
    gpu.unjitteredViewProj     = view.unjitteredViewProj;
    gpu.prevUnjitteredViewProj = view.prevUnjitteredViewProj;
    gpu.invViewProj            = view.invViewProj;

    const JPH::Vec4 camPos {view.cameraPosition, view.frameClock};
    Store(gpu.camPos, camPos);
    const JPH::Vec4 lightDir {view.sunDirection, view.sunIntensity};
    Store(gpu.lightDir, lightDir);

    gpu.sunRadiance = view.sunRadiance;
    gpu.sunSize     = view.sunSize;

    gpu.probeMin     = view.probeMin;
    gpu.probeMax     = view.probeMax;
    gpu.probePos     = view.probePos;
    gpu.jitterParams = view.jitterParams;

    gpu.enableRTR        = view.enableRTR;
    gpu.fullBright       = view.fullBright;
    gpu.shadowWidth      = view.shadowWidth;
    gpu.shadowResolution = view.shadowResolution;
    gpu.ambientExposure  = view.ambientExposure;

    gpu.skyZenith  = view.skyZenith;
    gpu.skyHorizon = view.skyHorizon;
    gpu.skyGround  = view.skyGround;
    return gpu;
}

} // namespace ZHLN
