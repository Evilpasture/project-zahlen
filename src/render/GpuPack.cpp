// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuPack.hpp"

#include <Zahlen/Math/ValueTypes.hpp>

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

    packed.gravity = desc.gravity;
    packed.drag    = desc.drag;

    packed.turbulence     = desc.turbulence;
    packed.turbulenceFreq = desc.turbulenceFreq;

    packed.spawnOrigin    = desc.spawnOrigin;
    packed.spawnRadius    = desc.spawnRadius;
    packed.spawnBoxExtent = desc.spawnBoxExtent;
    packed.loopBoundary   = desc.loopBoundary;

    packed.initVelMin  = desc.initVelMin;
    packed.lifetimeMin = desc.lifetimeMin;
    packed.initVelMax  = desc.initVelMax;
    packed.lifetimeMax = desc.lifetimeMax;

    packed.startColor = desc.startColor;
    packed.endColor   = desc.endColor;
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

    packed.gravity = desc.gravity;
    packed.drag    = desc.drag;

    packed.turbulence     = desc.turbulence;
    packed.turbulenceFreq = desc.turbulenceFreq;

    packed.spawnOrigin    = desc.spawnOrigin;
    packed.spawnRadius    = desc.spawnRadius;
    packed.spawnBoxExtent = desc.spawnBoxExtent;
    packed.loopBoundary   = desc.loopBoundary;

    packed.initVelMin  = desc.initVelMin;
    packed.lifetimeMin = desc.lifetimeMin;
    packed.initVelMax  = desc.initVelMax;
    packed.lifetimeMax = desc.lifetimeMax;

    packed.rotVelMin = desc.rotVelMin;
    packed.scaleMin  = desc.scaleMin;
    packed.rotVelMax = desc.rotVelMax;
    packed.scaleMax  = desc.scaleMax;

    packed.startColor = desc.startColor;
    packed.endColor   = desc.endColor;

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

    packed.position  = desc.position;
    packed.direction = desc.direction;
    packed.color     = desc.color;

    if (desc.type == LightType::Spot && desc.outerConeRadians > 0.0f) {
        const float outer   = std::min(desc.outerConeRadians, JPH::JPH_PI * 0.5f);
        const float inner   = JPH::Clamp(desc.innerConeRadians, 0.0f, outer);
        packed.outerConeCos = JPH::Cos(outer);
        packed.innerConeCos = JPH::Cos(inner);
    } else {
        packed.innerConeCos = -1.0f;
        packed.outerConeCos = -1.0f;
    }

    const JPH::Vec3 position = desc.position;
    JPH::Vec4(worldToView * position, 1.0f).StoreFloat4(&packed.positionView);

    if (desc.type == LightType::Area) {
        const JPH::Mat44 quad = desc.points;
        for (uint32_t column = 0; column < 4; ++column) {
            const ZHLN::Float4 lane = quad.GetColumn4(column);
            packed.points[column]   = lane;
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

    packed.camPos      = JPH::Float4 {frame.camPosition.x, frame.camPosition.y, frame.camPosition.z, frame.frameClock};
    packed.lightDir    = JPH::Float4 {frame.sunDirection.x, frame.sunDirection.y, frame.sunDirection.z, frame.sunIntensity};
    packed.sunRadiance = JPH::Float4 {frame.sunRadiance.x, frame.sunRadiance.y, frame.sunRadiance.z, 0.0f};

    packed.probeMin = JPH::Float4 {frame.probeMin.x, frame.probeMin.y, frame.probeMin.z, static_cast<float>(frame.useLocalProbe)};
    packed.probeMax = JPH::Float4 {frame.probeMax.x, frame.probeMax.y, frame.probeMax.z, 0.0f};
    packed.probePos = JPH::Float4 {frame.probePos.x, frame.probePos.y, frame.probePos.z, 0.0f};

    packed.jitterParams = frame.jitter;

    packed.enableRTR        = frame.enableRTR;
    packed.fullBright       = frame.fullBright;
    packed.frameIndex       = frame.frameIndex;
    packed.shadowWidth      = frame.shadowWidth;
    packed.shadowResolution = frame.shadowResolution;
    packed.sunSize          = frame.sunSize;
    packed.ambientExposure  = frame.ambientExposure;

    packed.skyZenith  = frame.skyZenith;
    packed.skyHorizon = frame.skyHorizon;
    packed.skyGround  = frame.skyGround;

    return packed;
}

} // namespace ZHLN::GpuPack
