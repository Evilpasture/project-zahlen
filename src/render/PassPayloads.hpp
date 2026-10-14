// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pass payloads that have no single pass to live in.
//
// Every payload that belongs to exactly one pass is declared next to that pass
// and asserted there -- see ParticleUpdatePass.cpp, MeshParticleUpdatePass.cpp
// and ForwardPass.cpp. What is left here is the set more than one translation
// unit fills: ObjectConstants is pushed by the gbuffer, forward, viewmodel,
// translucent, decal and shadow paths alike, and the rest are filled from
// wherever their pass happens to be recorded.
//
// These used to be nested in RenderContext::Impl, which made each of them a
// member of the renderer's private state and made RenderInternal.hpp -- the
// header 67 translation units include -- the only place they could be
// declared. None of them is renderer state. They are the shader ABI, and they
// are only typed by the code that fills a push block.

#include "GpuAbi.hpp"
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include <cstdint>

namespace ZHLN {

// The shared instance-draw prefix. Every scene draw pushes this in front of
// the frame addresses: which instance to read, and whether this draw is the
// shadow pass (which resolves the instance against the shadow instance table).
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

// The second-pass culling constants: the GPU cull walks the first pass's
// surviving candidates and re-tests them against the HiZ pyramid it just
// built, so it needs the pyramid's size as well as the view-projection.
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
    // The shader reads the skin stream as raw uint words (packed joint
    // indices + weight bits); there is no CPU-side element struct.
    Vk::DeviceAddress<uint32_t>           inSkinAddr;
    Vk::DeviceAddress<VertexPosition>     outPosAddr;
    Vk::DeviceAddress<VertexTangentFrame> outFrameAddr;
    Vk::DeviceAddress<JPH::Mat44>         jointsAddr; // shader GPUJoint: four float4 columns
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

// Every shared payload has to fit the push blob's prefix in front of the frame
// addresses. A payload that outgrows it is a compile error here, in the one
// place the shared ones are declared, rather than a silent truncation in
// BindHeapsAndPushFrame.
static_assert(
    (GpuAbi::ScenePassPayload<ObjectConstants> && GpuAbi::ScenePassPayload<UIObjectConstants> &&
     GpuAbi::ScenePassPayload<MeshParticleRenderPush> && GpuAbi::ScenePassPayload<SkinningConstants> &&
     GpuAbi::ScenePassPayload<BakePush> && GpuAbi::ScenePassPayload<ScenePassPushConstants>),
    "a shared pass payload no longer fits the push blob's prefix in front of the frame addresses"
);

} // namespace ZHLN
