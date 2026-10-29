// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuPack.hpp"

namespace ZHLN::GpuPack {

static_assert(
    static_cast<uint32_t>(LightType::Directional) == static_cast<uint32_t>(Slang::LightType::Directional)
    && static_cast<uint32_t>(LightType::Point) == static_cast<uint32_t>(Slang::LightType::Point)
    && static_cast<uint32_t>(LightType::Spot) == static_cast<uint32_t>(Slang::LightType::Spot)
    && static_cast<uint32_t>(LightType::Area) == static_cast<uint32_t>(Slang::LightType::Area)
    && static_cast<uint32_t>(LightType::Sun) == static_cast<uint32_t>(Slang::LightType::Sun)
    && static_cast<uint32_t>(ParticleAlignment::CameraBillboard) == static_cast<uint32_t>(Slang::ParticleAlignment::CameraBillboard)
    && static_cast<uint32_t>(ParticleAlignment::VelocityStretched) == static_cast<uint32_t>(Slang::ParticleAlignment::VelocityStretched)
    && static_cast<uint32_t>(ParticleAlignment::GroundFlat) == static_cast<uint32_t>(Slang::ParticleAlignment::GroundFlat),
    "the generated Slang GPU enums must preserve the host enums' wire values"
);

namespace {

[[nodiscard]] auto ToFloat4(const JPH::Vec4& v) noexcept -> JPH::Float4 {
    return JPH::Float4 {v.GetX(), v.GetY(), v.GetZ(), v.GetW()};
}

} // namespace

auto PackMeshlet(const MeshletDesc& desc) noexcept -> GPUMeshlet {
    GPUMeshlet packed {};
    packed.vertexOffset   = desc.vertexOffset;
    packed.triangleOffset = desc.triangleOffset;
    packed.vertexCount    = desc.vertexCount;
    packed.triangleCount  = desc.triangleCount;
    packed.sphereCenter   = JPH::Float3 {desc.sphereCenter[0], desc.sphereCenter[1], desc.sphereCenter[2]};
    packed.sphereRadius   = desc.sphereRadius;
    packed.coneApex       = JPH::Float3 {desc.coneApex[0], desc.coneApex[1], desc.coneApex[2]};
    packed.coneCutoff     = desc.coneCutoff;
    packed.coneAxis       = JPH::Float3 {desc.coneAxis[0], desc.coneAxis[1], desc.coneAxis[2]};
    return packed;
}

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

    packed.textureIndex = textureIndex;
    packed.alignment    = static_cast<Slang::ParticleAlignment>(static_cast<uint32_t>(desc.alignment));
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

auto PackLight(const LightDesc& desc, const JPH::Mat44& worldToView, int32_t shadowTileBase) noexcept -> Light {
    Light packed {};

    packed.type           = static_cast<Slang::LightType>(static_cast<uint32_t>(desc.type));
    packed.intensity      = desc.intensity;
    packed.radius         = desc.radius;
    packed.twoSided       = desc.twoSided;
    packed.baked          = desc.baked;
    packed.range          = (desc.range > 0.0f) ? desc.range : 1000.0f;
    packed.shadowTileBase = shadowTileBase;

    desc.position.StoreFloat3(&packed.position);
    desc.direction.StoreFloat3(&packed.direction);
    desc.color.StoreFloat3(&packed.color);

    if (desc.type == LightType::Spot && desc.outerConeRadians > 0.0f) {
        const float outer   = std::min(desc.outerConeRadians, JPH::JPH_PI * 0.5f);
        const float inner   = JPH::Clamp(desc.innerConeRadians, 0.0f, outer);
        packed.outerConeCos = JPH::Cos(outer);
        packed.innerConeCos = JPH::Cos(inner);
    } else {
        packed.innerConeCos = -1.0f;
        packed.outerConeCos = -1.0f;
    }

    JPH::Vec4(worldToView * desc.position, 1.0f).StoreFloat4(&packed.positionView);

    if (desc.type == LightType::Area) {
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

    packed.camPos      = JPH::Float4 {frame.camPosition.GetX(), frame.camPosition.GetY(), frame.camPosition.GetZ(), frame.frameClock};
    packed.lightDir    = JPH::Float4 {frame.sunDirection.GetX(), frame.sunDirection.GetY(), frame.sunDirection.GetZ(), frame.sunIntensity};
    packed.sunRadiance = JPH::Float4 {frame.sunRadiance.GetX(), frame.sunRadiance.GetY(), frame.sunRadiance.GetZ(), 0.0f};

    packed.probeMin = JPH::Float4 {frame.probeMin.GetX(), frame.probeMin.GetY(), frame.probeMin.GetZ(), static_cast<float>(frame.useLocalProbe)};
    packed.probeMax = JPH::Float4 {frame.probeMax.GetX(), frame.probeMax.GetY(), frame.probeMax.GetZ(), 0.0f};
    packed.probePos = JPH::Float4 {frame.probePos.GetX(), frame.probePos.GetY(), frame.probePos.GetZ(), 0.0f};

    packed.jitterParams = ToFloat4(frame.jitter);

    packed.enableRTR        = frame.enableRTR;
    packed.fullBright       = frame.fullBright;
    packed.frameIndex       = frame.frameIndex;
    packed.shadowWidth      = frame.shadowWidth;
    packed.shadowResolution = frame.shadowResolution;
    packed.sunSize          = frame.sunSize;
    packed.ambientExposure  = frame.ambientExposure;

    packed.skyZenith  = ToFloat4(frame.skyZenith);
    packed.skyHorizon = ToFloat4(frame.skyHorizon);
    packed.skyGround  = ToFloat4(frame.skyGround);

    return packed;
}

} // namespace ZHLN::GpuPack
