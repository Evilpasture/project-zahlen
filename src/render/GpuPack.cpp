// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuPack.hpp"

// Field-for-field, in the shader's order and the shader's lane types. Jolt's own
// StoreFloat3/StoreFloat4 do the SIMD-to-pod crossing, so nothing here is a memcpy
// and nothing here can be a surprising shape: a field named on the left is a field
// named on the right, and a missing one is a compile error rather than a zero the
// shader reads.

namespace ZHLN::GpuPack {

namespace {

[[nodiscard]] auto ToFloat4(const JPH::Vec4& v) noexcept -> JPH::Float4 {
    return JPH::Float4 {v.GetX(), v.GetY(), v.GetZ(), v.GetW()};
}

} // namespace

auto PackParticleEmitter(const ParticleEmitterDesc& desc, uint32_t textureIndex, uint32_t blendMode) noexcept -> ParticleEmitterParams {
    ParticleEmitterParams packed {};

    desc.gravity.StoreFloat3(&packed.gravity);
    packed.drag = desc.drag;

    desc.turbulence.StoreFloat3(&packed.turbulence);
    packed.turbulenceFreq = desc.turbulenceFreq;

    desc.spawnOrigin.StoreFloat3(&packed.spawnOrigin);
    packed.spawnRadius = desc.spawnRadius;
    desc.spawnBoxExtent.StoreFloat3(&packed.spawnBoxExtent);
    packed.loopBoundary = desc.loopBoundary;

    desc.initVelMin.StoreFloat3(&packed.initVelMin);
    packed.lifetimeMin = desc.lifetimeMin;
    desc.initVelMax.StoreFloat3(&packed.initVelMax);
    packed.lifetimeMax = desc.lifetimeMax;

    desc.startColor.StoreFloat4(&packed.startColor);
    desc.endColor.StoreFloat4(&packed.endColor);
    packed.startSize = desc.startSize;
    packed.endSize   = desc.endSize;

    packed.spinSpeed = desc.spinSpeed;
    // Renderer-owned fields: the author names a texture and a blend, not an index.
    packed.textureIndex = textureIndex;
    packed.alignment    = desc.alignment;
    packed.blendMode    = blendMode;

    return packed;
}

auto PackMeshParticleEmitter(const MeshParticleEmitterDesc& desc) noexcept -> MeshParticleEmitterParams {
    MeshParticleEmitterParams packed {};

    desc.gravity.StoreFloat3(&packed.gravity);
    packed.drag = desc.drag;

    desc.turbulence.StoreFloat3(&packed.turbulence);
    packed.turbulenceFreq = desc.turbulenceFreq;

    desc.spawnOrigin.StoreFloat3(&packed.spawnOrigin);
    packed.spawnRadius = desc.spawnRadius;
    desc.spawnBoxExtent.StoreFloat3(&packed.spawnBoxExtent);
    packed.loopBoundary = desc.loopBoundary;

    desc.initVelMin.StoreFloat3(&packed.initVelMin);
    packed.lifetimeMin = desc.lifetimeMin;
    desc.initVelMax.StoreFloat3(&packed.initVelMax);
    packed.lifetimeMax = desc.lifetimeMax;

    desc.rotVelMin.StoreFloat3(&packed.rotVelMin);
    packed.scaleMin = desc.scaleMin;
    desc.rotVelMax.StoreFloat3(&packed.rotVelMax);
    packed.scaleMax = desc.scaleMax;

    desc.startColor.StoreFloat4(&packed.startColor);
    desc.endColor.StoreFloat4(&packed.endColor);

    return packed;
}

auto PackLight(const LightDesc& desc, const JPH::Mat44& worldToView) noexcept -> Light {
    Light packed {};

    packed.type        = desc.type;
    packed.intensity   = desc.intensity;
    packed.radius      = desc.radius;
    packed.twoSided    = desc.twoSided;
    packed.range       = (desc.range > 0.0f) ? desc.range : 1000.0f;
    packed.shadowLayer = desc.shadowLayer;

    desc.position.StoreFloat3(&packed.position);
    desc.direction.StoreFloat3(&packed.direction);
    desc.color.StoreFloat3(&packed.color);

    // The shader reads a light's position in view space (w = 1, which the shader
    // ignores but costs nothing to define). The engine has the world position; the
    // view matrix is the renderer's, so this is the one place the two meet.
    JPH::Vec4(worldToView * desc.position, 1.0f).StoreFloat4(&packed.positionView);

    if (desc.type == LightType::Area) {
        // Four 16-byte lanes in the shader, one column of the quad per lane.
        for (uint32_t column = 0; column < 4; ++column) {
            desc.points.GetColumn4(column).StoreFloat4(&packed.points[column]);
        }
    }

    return packed;
}

auto PackFrameData(const FrameData& frame) noexcept -> FrameUniforms {
    FrameUniforms packed {};

    packed.viewProj               = frame.viewProj;
    packed.unjitteredViewProj     = frame.unjitteredViewProj;
    packed.prevUnjitteredViewProj = frame.prevUnjitteredViewProj;
    packed.invViewProj            = frame.invViewProj;

    // Named fields become lanes here and nowhere else: the clock rides camPos.w,
    // the sun's intensity rides lightDir.w, the probe flag rides probeMin.w. That
    // is the shader's layout, and this is the boundary it belongs to.
    packed.camPos = JPH::Float4 {frame.camPosition.GetX(), frame.camPosition.GetY(), frame.camPosition.GetZ(), frame.frameClock};
    packed.lightDir =
        JPH::Float4 {frame.sunDirection.GetX(), frame.sunDirection.GetY(), frame.sunDirection.GetZ(), frame.sunIntensity};
    packed.sunRadiance = JPH::Float4 {frame.sunRadiance.GetX(), frame.sunRadiance.GetY(), frame.sunRadiance.GetZ(), 0.0f};

    packed.probeMin = JPH::Float4 {frame.probeMin.GetX(), frame.probeMin.GetY(), frame.probeMin.GetZ(), static_cast<float>(frame.useLocalProbe)};
    packed.probeMax = JPH::Float4 {frame.probeMax.GetX(), frame.probeMax.GetY(), frame.probeMax.GetZ(), 0.0f};
    packed.probePos = JPH::Float4 {frame.probePos.GetX(), frame.probePos.GetY(), frame.probePos.GetZ(), 0.0f};

    packed.jitterParams = ToFloat4(frame.jitter);

    packed.enableRTR        = frame.enableRTR;
    packed.fullBright       = frame.fullBright;
    packed.shadowWidth      = frame.shadowWidth;
    packed.shadowResolution = frame.shadowResolution;
    packed.sunSize          = frame.sunSize;
    packed.ambientExposure  = frame.ambientExposure;

    packed.skyZenith  = ToFloat4(frame.skyZenith);
    packed.skyHorizon = ToFloat4(frame.skyHorizon);
    packed.skyGround  = ToFloat4(frame.skyGround);

    // Everything else in the shader's struct is the renderer's, and is filled by
    // RenderContext::SetFrameData after this returns.
    return packed;
}

} // namespace ZHLN::GpuPack
