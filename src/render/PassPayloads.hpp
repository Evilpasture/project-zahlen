// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "GpuAbi.hpp"
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include <cstdint>

namespace ZHLN {

struct ObjectConstants {
    uint32_t instanceId;
    uint32_t isShadowPass;
};
static_assert(sizeof(ObjectConstants) == 8);

struct UIObjectConstants {
    JPH::Mat44 orthoMatrix;
    uint64_t   posAddress;
    uint64_t   surfaceAddress;
    uint32_t   albedoIdx;
    uint32_t   isSDF;
    uint32_t   useTextureColor;
};
static_assert(sizeof(UIObjectConstants) == 96);
static_assert(offsetof(UIObjectConstants, surfaceAddress) == 72);

struct CullingConstants {
    JPH::Mat44 viewProj;
    float      hizScreenSize[2];
    uint32_t   maxHiZMipLevel;
    uint32_t   drawCount;
    uint32_t   passIndex;
};

struct MeshParticleRenderPush {
    VkDeviceAddress particleBufferAddr;
    VkDeviceAddress posAddress;
    JPH::Float4     baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4     emissiveFactor {0.0f, 0.0f, 0.0f, 1.0f};
    VkDeviceAddress tangentFrameAddress;
    VkDeviceAddress surfaceAddress;
    VkDeviceAddress iboAddress;

    uint32_t indexCount;
    uint32_t albedoIdx;
    uint32_t normalIdx;
    uint32_t pbrIdx;
    uint32_t emissiveIdx;
    float    roughness;
    float    metallic;
    float    alphaCutoff;
    uint32_t alphaMode;

    uint32_t samplerCodes0;
    uint32_t samplerCodes1;
    uint32_t unlit;
};
static_assert(sizeof(MeshParticleRenderPush) == 120);
static_assert(offsetof(MeshParticleRenderPush, baseColorFactor) == 16);
static_assert(offsetof(MeshParticleRenderPush, emissiveFactor) == 32);
static_assert(offsetof(MeshParticleRenderPush, tangentFrameAddress) == 48);
static_assert(offsetof(MeshParticleRenderPush, surfaceAddress) == 56);
static_assert(offsetof(MeshParticleRenderPush, indexCount) == 72);

struct alignas(8) SkinningConstants {
    Vk::DeviceAddress<VertexPosition>     inPosAddr;
    Vk::DeviceAddress<VertexTangentFrame> inFrameAddr;

    Vk::DeviceAddress<uint32_t>           inSkinAddr;
    Vk::DeviceAddress<VertexPosition>     outPosAddr;
    Vk::DeviceAddress<VertexTangentFrame> outFrameAddr;
    Vk::DeviceAddress<JPH::Mat44>         jointsAddr;
    Vk::DeviceAddress<JPH::Float4>        morphDeltasAddr;
    uint32_t                              vertexCount;
    uint32_t                              jointOffset;
    uint32_t                              morphOffset;
    uint32_t                              activeMorphCount;
    JPH::Float4                           morphWeights {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(offsetof(SkinningConstants, inFrameAddr) == 8);
static_assert(offsetof(SkinningConstants, outFrameAddr) == 32);

struct BakePush {
    uint32_t width;
    uint32_t height;
    float    scale;
    float    randomness;
    float    distortion;
};

using ScenePassPushConstants = GeneratedGpu::ScenePassPushConstants;

static_assert(
    (GpuAbi::ScenePassPayload<ObjectConstants> && GpuAbi::ScenePassPayload<UIObjectConstants> &&
     GpuAbi::ScenePassPayload<MeshParticleRenderPush> && GpuAbi::ScenePassPayload<SkinningConstants> &&
     GpuAbi::ScenePassPayload<BakePush> && GpuAbi::ScenePassPayload<ScenePassPushConstants>),
    "a shared pass payload no longer fits the push blob's prefix in front of the frame addresses"
);

} // namespace ZHLN
