// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>

namespace ZHLN {

namespace {
struct TaskSystemSchedulerAdapter {
    void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
        TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
    }
};
enum class RenderPassType : uint8_t { Main, Shadow };

[[nodiscard]] constexpr bool IsForwardOnly(uint32_t instanceFlags) noexcept {
    return (instanceFlags & 0xFF) == 2;
}

[[nodiscard]] inline bool IsVisibleIn(DrawFlags flags, RenderPassType passType) noexcept {
    using enum DrawFlags;
    const bool hasMain   = (flags & VisibleInMain) != None;
    const bool hasShadow = (flags & VisibleInShadow) != None;

    if (!hasMain && !hasShadow) {
        return true;
    }

    return (passType == RenderPassType::Main) ? hasMain : hasShadow;
}

template <typename T>
inline void SubmitDrawInstanced(
    Vk::CommandEncoder& encoder,
    const DrawCommand&  drawCmd,
    uint32_t            instanceIdx,
    VkDescriptorSet     bindlessSet,
    const T&            pushConstants,
    VkPipeline          pipelineOverride = VK_NULL_HANDLE,
    VkPipelineLayout    layoutOverride   = VK_NULL_HANDLE,
    VkShaderStageFlags  stages           = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
) noexcept {
    const auto* nativeMat = drawCmd.material;
    auto* const pipeline  = (pipelineOverride != VK_NULL_HANDLE) ? pipelineOverride : nativeMat->pipeline.Get();
    auto* const layout    = (layoutOverride != VK_NULL_HANDLE) ? layoutOverride : nativeMat->layout.Get();

    const uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;

    encoder.DrawInstanced(
        {.pipeline      = pipeline,
         .layout        = layout,
         .set           = bindlessSet,
         .vertexCount   = vertexCount,
         .instanceCount = 1,
         .firstVertex   = 0,
         .firstInstance = instanceIdx},
        pushConstants, stages
    );
}

void DrawCSGMeshes(const FrameRecorder& recorder, VkExtent3D extent) noexcept {
    VkCommandBuffer cmd = recorder.cmd;
    auto&           ctx = recorder.ctx;

    if (ctx.queues.csgDrawQueue.empty() || !ctx.csgWritePipeline.Valid()) {
        return;
    }

    ZHLN::ScopedTimer profTimer("GPU Stencil CSG Passes");

    for (const auto& csgCmd: ctx.queues.csgDrawQueue) {
        VkClearAttachment clearAttachment = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, .colorAttachment = {}, .clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}
        };
        VkClearRect clearRect = {
            .rect = {.offset = {.x = 0, .y = 0}, .extent = {.width = extent.width, .height = extent.height}}, .baseArrayLayer = 0, .layerCount = 1
        };
        vkCmdClearAttachments(cmd, 1, &clearAttachment, 1, &clearRect);

        for (const auto& cutter: csgCmd.cutters) {
            const ObjectConstants push = {.instanceId = cutter.instanceIdx, .isShadowPass = 0};
            SubmitDrawInstanced(
                recorder.encoder, cutter.draw, cutter.instanceIdx, recorder.bindlessSet, push, ctx.csgWritePipeline.Get(), ctx.csgPipelineLayout.Get()
            );
        }

        VkPipeline activePipeline = ctx.csgDifferencePipeline.Get();
        if (!csgCmd.cutters.empty() && csgCmd.cutters[0].operation == CSGOperation::Intersection) {
            activePipeline = ctx.csgIntersectionPipeline.Get();
        }

        const ObjectConstants push = {.instanceId = csgCmd.eyeInstanceIdx, .isShadowPass = 0};
        SubmitDrawInstanced(recorder.encoder, csgCmd.eyeDraw, csgCmd.eyeInstanceIdx, recorder.bindlessSet, push, activePipeline, ctx.csgPipelineLayout.Get());
    }
}

void Draw3DParticles(const FrameRecorder& recorder) noexcept {
    auto& ctx = recorder.ctx;
    if (!ctx.meshParticleRenderPipeline.Valid() || ctx.queues.meshParticleQueue.empty()) {
        return;
    }

    for (const auto& emitter: ctx.queues.meshParticleQueue) {
        auto*           pBuf    = ctx.meshPool.Resolve(emitter.gpuBuffer).value_or(nullptr);
        const Mesh*     gpuMesh = ctx.assetMeshMap.Find(emitter.meshAsset);
        const Material* gpuMat  = ctx.assetMaterialMap.Find(emitter.materialAsset);

        if ((pBuf == nullptr) || gpuMesh == nullptr || gpuMat == nullptr) {
            continue;
        }

        auto* posMesh  = ctx.meshPool.Resolve(gpuMesh->posBuffer).value_or(nullptr);
        auto* attrMesh = ctx.meshPool.Resolve(gpuMesh->attrBuffer).value_or(nullptr);
        auto* iboMesh  = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.meshPool.Resolve(gpuMesh->indexBuffer).value_or(nullptr) : nullptr;

        RenderContext::Impl::MeshParticleRenderPush rpc = {
            .particleBufferAddr = ctx.BufferAddress(pBuf->buffer.Handle()),
            .posAddress         = (posMesh != nullptr) ? posMesh->vboAddress : 0,
            .attrAddress        = (attrMesh != nullptr) ? attrMesh->vboAddress : 0,
            .iboAddress         = (iboMesh != nullptr) ? iboMesh->vboAddress : 0,
            .baseColorFactor    = {},
            .emissiveFactor     = {},
            .indexCount         = gpuMesh->indexCount,
            .albedoIdx          = ctx.textureManager.GetBindlessIndex(gpuMat->albedoMap),
            .normalIdx          = ctx.textureManager.GetBindlessIndex(gpuMat->normalMap),
            .pbrIdx             = ctx.textureManager.GetBindlessIndex(gpuMat->pbrMap),
            .emissiveIdx        = ctx.textureManager.GetBindlessIndex(gpuMat->emissiveMap),
            .roughness          = gpuMat->roughnessFactor,
            .metallic           = gpuMat->metallicFactor,
            .alphaCutoff        = gpuMat->alphaCutoff,
            .alphaMode          = gpuMat->alphaMode,
            .cascadeIndex       = 0
        };
        std::memcpy(rpc.baseColorFactor, gpuMat->baseColorFactor, sizeof(float) * 4);
        std::memcpy(rpc.emissiveFactor, gpuMat->emissiveFactor, sizeof(float) * 4);

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        recorder.encoder.DrawInstanced(
            {.pipeline      = ctx.meshParticleRenderPipeline.Get(),
             .layout        = ctx.meshParticleRenderLayout.Get(),
             .set           = recorder.bindlessSet,
             .vertexCount   = drawVertexCount,
             .instanceCount = emitter.maxParticles,
             .firstVertex   = 0,
             .firstInstance = 0},
            rpc
        );
    }
}

void Draw3DParticleShadows(const FrameRecorder& recorder, uint32_t cascadeIndex) noexcept {
    auto& ctx = recorder.ctx;
    if (!ctx.meshParticleShadowPipeline.Valid() || ctx.queues.meshParticleQueue.empty()) {
        return;
    }

    for (const auto& emitter: ctx.queues.meshParticleQueue) {
        auto*           pBuf    = ctx.meshPool.Resolve(emitter.gpuBuffer).value_or(nullptr);
        const Mesh*     gpuMesh = ctx.assetMeshMap.Find(emitter.meshAsset);
        const Material* gpuMat  = ctx.assetMaterialMap.Find(emitter.materialAsset);

        if ((pBuf == nullptr) || gpuMesh == nullptr || gpuMat == nullptr) {
            continue;
        }

        auto* posMesh = ctx.meshPool.Resolve(gpuMesh->posBuffer).value_or(nullptr);
        auto* iboMesh = (gpuMesh->indexBuffer != BufferHandle::Invalid) ? ctx.meshPool.Resolve(gpuMesh->indexBuffer).value_or(nullptr) : nullptr;

        RenderContext::Impl::MeshParticleRenderPush rpc = {
            .particleBufferAddr = ctx.BufferAddress(pBuf->buffer.Handle()),
            .posAddress         = (posMesh != nullptr) ? posMesh->vboAddress : 0,
            .attrAddress        = 0,
            .iboAddress         = (iboMesh != nullptr) ? iboMesh->vboAddress : 0,
            .baseColorFactor    = {},
            .emissiveFactor     = {},
            .indexCount         = gpuMesh->indexCount,
            .albedoIdx          = ctx.textureManager.GetBindlessIndex(gpuMat->albedoMap),
            .normalIdx          = 0,
            .pbrIdx             = 0,
            .emissiveIdx        = 0,
            .roughness          = 0.0f,
            .metallic           = 0.0f,
            .alphaCutoff        = gpuMat->alphaCutoff,
            .alphaMode          = gpuMat->alphaMode,
            .cascadeIndex       = cascadeIndex
        };
        std::memcpy(rpc.baseColorFactor, gpuMat->baseColorFactor, sizeof(float) * 4);

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        recorder.encoder.DrawInstanced(
            {.pipeline      = ctx.meshParticleShadowPipeline.Get(),
             .layout        = ctx.meshParticleRenderLayout.Get(),
             .set           = recorder.bindlessSet,
             .vertexCount   = drawVertexCount,
             .instanceCount = emitter.maxParticles,
             .firstVertex   = 0,
             .firstInstance = 0},
            rpc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        );
    }
}

struct GpuCullingPolicyPass1 {
    static void Record(
        const FrameRecorder&                                             recorder,
        const ZHLN::Array<GroupRange>&                                   groups,
        uint32_t                                                         drawCount,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         color_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         vel_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth_att
    ) noexcept {
        VkCommandBuffer cmd = recorder.cmd;
        auto&           ctx = recorder.ctx;

        // Transition buffer access to CLEAR / TRANSFER_WRITE
        VkBufferMemoryBarrier2 fillBarrier = {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = ctx.frames.secondPassCountBuffers[recorder.frameIndex].Handle(),
            .offset              = 0,
            .size                = VK_WHOLE_SIZE
        };
        Vk::BufferBarrier(cmd, fillBarrier);

        Vk::FillBuffer(cmd, ctx.frames.secondPassCountBuffers[recorder.frameIndex], 0, 0u);

        VkBufferMemoryBarrier2 clearBarrier = {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = ctx.frames.secondPassCountBuffers[recorder.frameIndex].Handle(),
            .offset              = 0,
            .size                = VK_WHOLE_SIZE
        };
        Vk::BufferBarrier(cmd, clearBarrier);

        // 2. Dispatch Culling Pass 1 (Frustum + Last Frame Hi-Z)
        struct CullingConstants {
            JPH::Mat44 viewProj;
            float      hizScreenSize[2];
            uint32_t   maxHiZMipLevel;
            uint32_t   drawCount;
            uint32_t   passIndex;
        } pc {};

        pc.viewProj         = ctx.unjittered_view_proj;
        pc.hizScreenSize[0] = (float) color_att.extent.width;
        pc.hizScreenSize[1] = (float) color_att.extent.height;
        pc.maxHiZMipLevel   = ctx.graphResources.hizMap.mipLevels > 0 ? ctx.graphResources.hizMap.mipLevels - 1 : 0;
        pc.drawCount        = drawCount;
        pc.passIndex        = 0; // PASS 1

        ctx.cullingPass.Dispatch(cmd, ctx.frames.cullingSetsPass1[recorder.frameIndex], (drawCount + 63) / 64, 1, 1, pc);

        using enum Vk::BarrierStage;
        using enum Vk::BarrierAccess;
        Vk::BeginBarrier<Compute, ShaderWrite>(Vk::CommandBuffer<Vk::QueueType::Graphics> {cmd}).TransitionTo<Indirect, IndirectRead>();

        // 3. Render Pass 1 Geometry
        Vk::DynamicPass(color_att.extent)
            .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
            .AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
            .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
            .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
            .Execute(cmd, [&]() {
                for (const auto& group: groups) {
                    if (!group.material->pipeline.Valid()) {
                        continue;
                    }
                    recorder.encoder.DrawIndirect(
                        {
                            .pipeline       = group.material->pipeline.Get(),
                            .layout         = group.material->layout.Get(),
                            .set            = recorder.bindlessSet,
                            .argumentBuffer = ctx.frames.indirectCommandsBuffers[recorder.frameIndex].Handle(),
                            .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                            .drawCount      = group.count,
                        },
                        ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                    );
                }
            });
    }
};

struct GpuCullingPolicyPass2 {
    static void Record(
        const FrameRecorder&                                             recorder,
        const ZHLN::Array<GroupRange>&                                   groups,
        uint32_t                                                         drawCount,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         color_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         vel_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth_att
    ) noexcept {
        VkCommandBuffer cmd = recorder.cmd;
        auto&           ctx = recorder.ctx;

        VkBufferMemoryBarrier2 fillBarrier = {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex].Handle(),
            .offset              = 0,
            .size                = VK_WHOLE_SIZE
        };
        Vk::BufferBarrier(cmd, fillBarrier);

        Vk::FillBuffer(cmd, ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex], 0, 0u);

        VkBufferMemoryBarrier2 clearBarrier = {
            .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext               = nullptr,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex].Handle(),
            .offset              = 0,
            .size                = VK_WHOLE_SIZE
        };
        Vk::BufferBarrier(cmd, clearBarrier);

        // 2. Dispatch Culling Pass 2 (Current Frame Hi-Z Re-test)

        RenderContext::Impl::CullingConstants pc {
            .viewProj       = ctx.unjittered_view_proj,
            .hizScreenSize  = {(float) color_att.extent.width, (float) color_att.extent.height},
            .maxHiZMipLevel = ctx.graphResources.hizMap.mipLevels > 0 ? ctx.graphResources.hizMap.mipLevels - 1 : 0,
            .drawCount      = drawCount,
            .passIndex      = 1,
        };
        ctx.cullingPass.Dispatch(cmd, ctx.frames.cullingSetsPass2[recorder.frameIndex], (drawCount + 63) / 64, 1, 1, pc);

        using enum Vk::BarrierStage;
        using enum Vk::BarrierAccess;
        Vk::BeginBarrier<Compute, ShaderWrite>(Vk::CommandBuffer<Vk::QueueType::Graphics> {cmd}).TransitionTo<Indirect, IndirectRead>();

        // 3. Render Pass 2 Geometry (Newly Unoccluded) with LOAD_OP_LOAD!
        Vk::DynamicPass(color_att.extent)
            .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .Execute(cmd, [&]() {
                for (const auto& group: groups) {
                    if (!group.material->pipeline.Valid()) {
                        continue;
                    }
                    recorder.encoder.DrawIndirect(
                        {
                            .pipeline       = group.material->pipeline.Get(),
                            .layout         = group.material->layout.Get(),
                            .set            = recorder.bindlessSet,
                            .argumentBuffer = ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex].Handle(),
                            .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                            .drawCount      = group.count,
                        },
                        ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                    );
                }
                // Particles and CSG are drawn ONLY in Pass 2 to avoid double rendering
                DrawCSGMeshes(recorder, color_att.extent);
                Draw3DParticles(recorder);
            });
    }
};

struct CpuCullingPolicyPass1 {
    static void Record(
        const FrameRecorder& recorder,
        const ZHLN::Array<GroupRange>& /*groups*/,
        uint32_t                                                         drawCount,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         color_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         vel_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth_att
    ) noexcept {
        VkCommandBuffer cmd          = recorder.cmd;
        auto&           ctx          = recorder.ctx;
        const auto&     colorFormats = ActiveGBuffer::array;

        Vk::DynamicPass(color_att.extent)
            .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
            .AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
            .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
            .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
            .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
            .Execute(cmd, [&]() {
                Vk::ParallelDrawDispatch(
                    cmd, Vk::SecondaryInheritance {.colorFormats = colorFormats, .depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT},
                    {.width = color_att.extent.width, .height = color_att.extent.height}, drawCount, kParallelChunkSize, TaskSystemSchedulerAdapter {},
                    [&](uint32_t /*chunkIdx*/) -> VkCommandBuffer {
                        uint32_t wIdx = TaskSystem::GetWorkerIndex();
                        if (wIdx >= ctx.workerCmds.size())
                            wIdx = (uint32_t) (ctx.workerCmds.size() - 1);
                        uint32_t localCmdIdx = ctx.workerCmds[wIdx].cmdCount[recorder.frameIndex].fetch_add(1, std::memory_order::relaxed);
                        return ctx.workerCmds[wIdx].pools[recorder.frameIndex][localCmdIdx];
                    },
                    [&](Vk::CommandEncoder& encoder, uint32_t i) {
                        const auto& drawCmd = ctx.queues.drawQueue[i];
                        if (!IsVisibleIn(drawCmd.flags, RenderPassType::Main) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None ||
                            !drawCmd.material->pipeline.Valid() || IsForwardOnly(drawCmd.instanceData.flags)) {
                            return;
                        }
                        SubmitDrawInstanced(encoder, drawCmd, i, recorder.bindlessSet, ObjectConstants {.instanceId = i, .isShadowPass = 0});
                    }
                );
            });
    }
};

struct CpuCullingPolicyPass2 {
    static void Record(
        const FrameRecorder& recorder,
        const ZHLN::Array<GroupRange>& /*groups*/,
        uint32_t /*drawCount*/,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         color_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         vel_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth_att
    ) noexcept {
        // CPU Culling does everything in Pass 1. Pass 2 just draws CSG and Particles on top.
        VkCommandBuffer cmd = recorder.cmd;
        Vk::DynamicPass(color_att.extent)
            .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .Execute(cmd, [&]() {
                DrawCSGMeshes(recorder, color_att.extent);
                Draw3DParticles(recorder);
            });
    }
};

template <typename CullingPolicy, typename... Args>
void ExecutePass(const FrameRecorder& recorder, const ZHLN::Array<GroupRange>& groups, uint32_t drawCount, Args&&... args) {
    CullingPolicy::Record(recorder, groups, drawCount, std::forward<Args>(args)...);
}
} // namespace
namespace Passes {

void ShadowPass::Execute(const FrameRecorder& recorder) const noexcept {
    using enum LightType;
    using enum RenderPassType;
    VkCommandBuffer cmd = recorder.cmd;
    auto&           ctx = recorder.ctx;

    std::array<Frustum, RenderContext::Impl::NUM_CASCADES> cascadeFrustums {};
    for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
        cascadeFrustums[c].Update(ctx.currentUniforms.lightSpaceMatrices[c]);
    }

    auto  mapped           = ctx.frames.shadowIndirectBuffers->Map();
    auto* indirectCmdsBase = static_cast<VkDrawIndirectCommand*>(mapped.data);

    std::array<uint32_t, 8> passWriteOffsets {};
    for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
        passWriteOffsets[c] = c * kGpuCullingMaxInstances;
    }
    for (uint32_t l = 0; l < RenderContext::Impl::MAX_PUNCTUAL_LIGHTS; ++l) {
        passWriteOffsets[4 + l] = (4 + l) * kGpuCullingMaxInstances;
    }

    std::array<uint32_t, 8> passDrawCounts {};

    std::array<const GPULight*, RenderContext::Impl::MAX_PUNCTUAL_LIGHTS> activeShadowLights {};
    uint32_t                                                              activeShadowLightCount = 0;
    for (const auto& light: ctx.mappedLights) {
        if (light.shadowLayer >= 0 && light.type == Point) {
            activeShadowLights[activeShadowLightCount++] = &light;
            if (activeShadowLightCount >= RenderContext::Impl::MAX_PUNCTUAL_LIGHTS) {
                break;
            }
        }
    }

    for (uint32_t i = 0; i < ctx.queues.drawQueue.size(); ++i) {
        const auto& drawCmd = ctx.queues.drawQueue[i];

        if (!IsVisibleIn(drawCmd.flags, Shadow) || IsForwardOnly(drawCmd.instanceData.flags)) {
            continue;
        }

        uint32_t  vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;
        JPH::Vec3 meshPos     = drawCmd.instanceData.world.GetTranslation();
        float     radius      = drawCmd.instanceData.cullRadius;

        // Cascade Culling: Only assign to cascades that geometrically intersect the mesh!
        for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
            if (cascadeFrustums[c].IsSphereVisible(meshPos, radius)) {
                uint32_t writeIdx          = passWriteOffsets[c] + passDrawCounts[c];
                indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
                passDrawCounts[c]++;
            }
        }

        // Punctual light logic
        for (uint32_t l = 0; l < activeShadowLightCount; ++l) {
            const auto* light   = activeShadowLights[l];
            uint32_t    slotIdx = 4 + light->shadowLayer;
            if (slotIdx >= 8) {
                continue;
            }

            JPH::Vec3 lightPos(light->position[0], light->position[1], light->position[2]);
            float     distToLightSq = (meshPos - lightPos).LengthSq();
            float     maxRange      = light->range + radius;

            if (distToLightSq <= (maxRange * maxRange)) {
                uint32_t pWriteIdx          = passWriteOffsets[slotIdx] + passDrawCounts[slotIdx];
                indirectCmdsBase[pWriteIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
                passDrawCounts[slotIdx]++;
            }
        }
    }

    {
        Profiler::ScopedGpuProfile timer(cmd, recorder.frameIndex, ctx.gpuProfiler, Stage::ShadowPass);
        bool                       hasMeshParticles = !ctx.queues.meshParticleQueue.empty();

        for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
            uint32_t csmDrawCount = passDrawCounts[c];

            // Render exclusively to this cascade's slice/layer
            Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> cascadeLayerImage = {
                .handle = ctx.graphResources.shadowMap.image.Handle(),
                .view   = ctx.shadowCascadeViews[c].Get(),
                .extent = {.width = ctx.graphResources.shadowMap.extent.width, .height = ctx.graphResources.shadowMap.extent.height, .depth = 1},
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT
            };

            Vk::DynamicPass(cascadeLayerImage.extent)
                .AddDepth(cascadeLayerImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kShadowClearDepth)
                .Execute(cmd, [&]() {
                    if (csmDrawCount > 0) {
                        recorder.encoder.DrawIndirect(
                            {.pipeline       = ctx.shadowPipeline.Get(),
                             .layout         = ctx.shadowPipelineLayout.Get(),
                             .set            = recorder.bindlessSet,
                             .argumentBuffer = ctx.frames.shadowIndirectBuffers->Handle(),
                             .offset         = Vk::DrawIndirectState::OffsetForIndex(passWriteOffsets[c]),
                             .drawCount      = csmDrawCount},
                            ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 1 + c}, // Map: 1 -> Cas0, 2 -> Cas1, etc.
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                        );
                    }

                    if (hasMeshParticles) {
                        Draw3DParticleShadows(recorder, c);
                    }
                });
        }
    }

    if (ctx.punctualShadowPipeline.Valid() && !ctx.punctualShadowViews.empty()) {
        auto ExecutePunctualPass = [&](const Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>& subViewImage, auto&& recordFn) {
            Vk::DynamicPass(subViewImage.extent)
                .ViewMask(kCubemapFaceMask)
                .AddDepth(subViewImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kShadowClearDepth)
                .Execute(cmd, std::forward<decltype(recordFn)>(recordFn));
        };

        for (uint32_t l_idx = 0; l_idx < ctx.mappedLights.size(); ++l_idx) {
            const auto& light = ctx.mappedLights[l_idx];
            if (light.shadowLayer < 0) {
                continue;
            }

            uint32_t slotIdx   = 4 + light.shadowLayer;
            uint32_t drawCount = passDrawCounts[slotIdx];

            if (drawCount == 0 && ctx.queues.meshParticleQueue.empty()) {
                continue;
            }

            Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> subViewImage = {
                .handle = ctx.graphResources.shadowAtlas.image.Handle(),
                .view   = ctx.punctualShadowViews[light.shadowLayer].Get(),
                .extent = {.width = 1024, .height = 1024, .depth = {}},
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT
            };

            ExecutePunctualPass(subViewImage, [&]() {
                if (drawCount > 0) {
                    const struct PunctualPush {
                        uint32_t lightIdx;
                    } pc = {l_idx};

                    recorder.encoder.DrawIndirect(
                        {
                            .pipeline       = ctx.punctualShadowPipeline.Get(),
                            .layout         = ctx.punctualShadowPipelineLayout.Get(),
                            .set            = recorder.bindlessSet,
                            .argumentBuffer = ctx.frames.shadowIndirectBuffers->Handle(),
                            .offset         = Vk::DrawIndirectState::OffsetForIndex(passWriteOffsets[slotIdx]),
                            .drawCount      = drawCount,
                        },
                        pc, VK_SHADER_STAGE_VERTEX_BIT
                    );
                }
            });
        }
    }
}

void MainPass1::Execute(
    const FrameRecorder&                                                                                       recorder,
    SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> in
) const noexcept {
    auto                       cmd = recorder.cmd;
    auto&                      ctx = recorder.ctx;
    Profiler::ScopedGpuProfile timer(cmd, recorder.frameIndex, ctx.gpuProfiler, Stage::MainPass1);

    const auto drawCount = static_cast<uint32_t>(ctx.queues.drawQueue.size());
    if (drawCount == 0) {
        Vk::DynamicPass(in.sceneColor.extent)
            .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
            .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
            .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
            .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
            .Execute(cmd, []() {});
        return;
    }

    ZHLN::Array<GroupRange> groups;
    groups.reserve((drawCount + 15) / 16);
    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < drawCount; ++i) {
        const auto&       drawCmd = ctx.queues.drawQueue[i];
        const auto* const drawMat = drawCmd.material;

        if (IsForwardOnly(drawCmd.instanceData.flags) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None || !drawMat->pipeline.Valid()) {
            currentPipeline = VK_NULL_HANDLE;
            continue;
        }

        if (i == 0 || drawMat->pipeline.Get() != currentPipeline) {
            groups.push_back(GroupRange {.material = drawMat, .start = i, .count = 1});
            currentPipeline = drawMat->pipeline.Get();
        } else {
            groups.back().count++;
        }
    }

    const bool useGpuCulling =
        ctx.cullingPass.pipeline.Valid() && ctx.frames.indirectCommandsBuffers->Valid() && (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling();
    if (useGpuCulling) {
        ExecutePass<GpuCullingPolicyPass1>(recorder, groups, drawCount, in.sceneColor, in.velocity, in.normRough, in.depth);
    } else {
        ExecutePass<CpuCullingPolicyPass1>(recorder, groups, drawCount, in.sceneColor, in.velocity, in.normRough, in.depth);
    }
}

void MainPass2::Execute(
    const FrameRecorder&                                                                                       recorder,
    SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> in
) const noexcept {
    auto                       cmd = recorder.cmd;
    auto&                      ctx = recorder.ctx;
    Profiler::ScopedGpuProfile timer(cmd, recorder.frameIndex, ctx.gpuProfiler, Stage::MainPass2);

    const auto drawCount = static_cast<uint32_t>(ctx.queues.drawQueue.size());
    if (drawCount == 0 && ctx.queues.meshParticleQueue.empty() && ctx.queues.csgDrawQueue.empty()) {
        return;
    }

    ZHLN::Array<GroupRange> groups;
    groups.reserve((drawCount + 15) / 16);
    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < drawCount; ++i) {
        const auto&       drawCmd = ctx.queues.drawQueue[i];
        const auto* const drawMat = drawCmd.material;

        if (IsForwardOnly(drawCmd.instanceData.flags) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None || !drawMat->pipeline.Valid()) {
            currentPipeline = VK_NULL_HANDLE;
            continue;
        }

        if (i == 0 || drawMat->pipeline.Get() != currentPipeline) {
            groups.push_back(GroupRange {.material = drawMat, .start = i, .count = 1});
            currentPipeline = drawMat->pipeline.Get();
        } else {
            groups.back().count++;
        }
    }

    const bool useGpuCulling =
        ctx.cullingPass.pipeline.Valid() && ctx.frames.indirectCommandsBuffers->Valid() && (drawCount <= kGpuCullingMaxInstances) && !Diag::DisableGpuCulling();
    if (useGpuCulling) {
        ExecutePass<GpuCullingPolicyPass2>(recorder, groups, drawCount, in.sceneColor, in.velocity, in.normRough, in.depth);
    } else {
        ExecutePass<CpuCullingPolicyPass2>(recorder, groups, drawCount, in.sceneColor, in.velocity, in.normRough, in.depth);
    }
}

void TranslucentPrePass::Execute(
    const FrameRecorder&                                             recorder,
    Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         norm_att,
    Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth_att
) const noexcept {
    VkCommandBuffer cmd = recorder.cmd;
    auto&           ctx = recorder.ctx;

    Vk::DynamicPass(norm_att.extent)
        .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
        .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < ctx.queues.drawQueue.size(); ++i) {
                const auto& drawCmd = ctx.queues.drawQueue[i];

                if ((drawCmd.instanceData.flags & 0xFF) != 2) {
                    continue;
                }

                if (drawCmd.prePassMaterial == nullptr || !drawCmd.prePassMaterial->pipeline.Valid()) {
                    continue;
                }

                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                SubmitDrawInstanced(
                    recorder.encoder, drawCmd, static_cast<uint32_t>(i), recorder.bindlessSet, push, drawCmd.prePassMaterial->pipeline.Get(),
                    drawCmd.prePassMaterial->layout.Get()
                );
            }
        });
}

void ForwardPass::Execute(
    const FrameRecorder&                                             recorder,
    Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         litColor,
    Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth
) const noexcept {
    VkCommandBuffer cmd = recorder.cmd;
    const auto&     ctx = recorder.ctx;

    Vk::DynamicPass(litColor.extent)
        .AddColor(litColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < ctx.queues.drawQueue.size(); ++i) {
                const auto& drawCmd = ctx.queues.drawQueue[i];

                if ((drawCmd.instanceData.flags & 0xFF) != 2) {
                    continue;
                }

                if (!drawCmd.material->pipeline.Valid()) {
                    continue;
                }

                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                SubmitDrawInstanced(recorder.encoder, drawCmd, static_cast<uint32_t>(i), recorder.bindlessSet, push);
            }

            if (ctx.particleRenderPipeline.Valid() && !ctx.queues.particleEmittersQueue.empty()) {
                auto* bindlessSet = ctx.frames.bindlessSets[ctx.frame_index];

                for (const auto& emitter: ctx.queues.particleEmittersQueue) {
                    auto* buffer = ctx.meshPool.Resolve(emitter.gpuBuffer).value_or(nullptr);
                    if (!buffer) {
                        continue;
                    }

                    RenderContext::Impl::ParticleRenderPushConstants pc = {
                        .particleBufferAddr = ctx.BufferAddress(buffer->buffer.Handle()),
                        .alignment          = static_cast<uint32_t>(emitter.params.alignment),
                        .textureIndex       = emitter.params.textureIndex
                    };

                    recorder.encoder.DrawInstanced(
                        {.pipeline      = ctx.particleRenderPipeline.Get(),
                         .layout        = ctx.particleRenderLayout.Get(),
                         .set           = bindlessSet,
                         .vertexCount   = 6,
                         .instanceCount = emitter.maxParticles,
                         .firstVertex   = 0,
                         .firstInstance = 0},
                        pc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
                }
            }

            if (ctx.linePipeline.Valid() && ctx.activeLineVertexCount > 0) {
                const ObjectConstants pc = {.instanceId = ctx.lineInstanceId, .isShadowPass = 0};

                recorder.encoder.DrawInstanced(
                    {.pipeline      = ctx.linePipeline.Get(),
                     .layout        = ctx.linePipelineLayout.Get(),
                     .set           = recorder.bindlessSet,
                     .vertexCount   = ctx.activeLineVertexCount,
                     .instanceCount = 1,
                     .firstVertex   = 0,
                     .firstInstance = ctx.lineInstanceId},
                    pc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                );
            }
        });
}

void BlitPass::Execute(
    const FrameRecorder&                                     recorder,
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
    Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> swapchainTarget,
    int                                                      fullBright
) const noexcept {
    VkCommandBuffer cmd = recorder.cmd;
    auto&           ctx = recorder.ctx;

    Profiler::ScopedGpuProfile timer(cmd, recorder.frameIndex, ctx.gpuProfiler, Stage::BlitPass);

    struct BlitPushConstants {
        float vignetteIntensity;
        float vignettePower;
        int   fullBright;
    } pc = {.vignetteIntensity = ctx.giSettings.vignetteIntensity, .vignettePower = ctx.giSettings.vignettePower, .fullBright = fullBright};

    if (ctx.blitPass.pipeline.Valid()) {
        Vk::DynamicPass(inColor.extent).AddColor(swapchainTarget, VK_ATTACHMENT_LOAD_OP_DONT_CARE).Execute(cmd, [&]() {
            ctx.blitPass.Execute(cmd, pc);

            if (!ctx.queues.uiBatches.empty()) {
                UIObjectConstants uipc {};
                uipc.orthoMatrix = Math::CreateOrthoMatrix(inColor.extent.width, inColor.extent.height);

                VkRect2D defaultScissor = {.offset = {.x = 0, .y = 0}, .extent = {.width = inColor.extent.width, .height = inColor.extent.height}};

                auto   baseVboAddress = ctx.frames.uiVboAddresses[recorder.frameIndex];
                size_t maxVertices    = ctx.frames.uiVbos[recorder.frameIndex].Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));

                for (const auto& batch: ctx.queues.uiBatches) {
                    uipc.albedoIdx   = ctx.textureManager.GetBindlessIndex(batch.texture);
                    uipc.isSDF       = batch.isSDF ? 1 : 0;
                    uipc.posAddress  = baseVboAddress + (batch.vertexStart * sizeof(VertexPosition));
                    uipc.attrAddress = baseVboAddress + (maxVertices * sizeof(VertexPosition)) + (batch.vertexStart * sizeof(VertexAttributes));

                    Vk::ScopedScissor scissorGuard(
                        cmd, {.target   = batch.useScissor ?
                                              VkRect2D {
                                                  .offset = {.x = batch.scissorRect.x, .y = batch.scissorRect.y},
                                                  .extent = {.width = batch.scissorRect.width, .height = batch.scissorRect.height}
                                              } :
                                              defaultScissor,
                              .fallback = defaultScissor}
                    );

                    recorder.encoder.DrawInstanced(
                        {.pipeline      = ctx.uiPipeline.Get(),
                         .layout        = ctx.uiPipelineLayout.Get(),
                         .set           = recorder.bindlessSet,
                         .vertexCount   = batch.vertexCount,
                         .instanceCount = 1,
                         .firstVertex   = 0,
                         .firstInstance = 0},
                        uipc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
                }
            }
            if (!ctx.window.IsTTY()) {
                ImGui::Render();
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            }
        });
    }
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swapchainTarget.handle);
}

void ViewmodelPass::Execute(
    const FrameRecorder&                                                                                       recorder,
    SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> in
) const noexcept {
    auto  cmd = recorder.cmd;
    auto& ctx = recorder.ctx;

    bool hasViewmodelDraws = false;
    for (const auto& drawCmd: ctx.queues.drawQueue) {
        if ((drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None && !IsForwardOnly(drawCmd.instanceData.flags)) {
            hasViewmodelDraws = true;
            break;
        }
    }

    if (!hasViewmodelDraws) {
        return;
    }

    Profiler::ScopedGpuProfile timer(cmd, recorder.frameIndex, ctx.gpuProfiler, Stage::ViewmodelPass);

    Vk::DynamicPass(in.sceneColor.extent)
        .AddColor(in.sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.velocity, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddColor(in.normRough, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(in.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < ctx.queues.drawQueue.size(); ++i) {
                const auto& drawCmd = ctx.queues.drawQueue[i];

                if ((drawCmd.flags & DrawFlags::Viewmodel) == DrawFlags::None || IsForwardOnly(drawCmd.instanceData.flags)) {
                    continue;
                }

                if (!drawCmd.material->pipeline.Valid()) {
                    continue;
                }

                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};
                SubmitDrawInstanced(recorder.encoder, drawCmd, static_cast<uint32_t>(i), recorder.bindlessSet, push);
            }
        });
}

} // namespace Passes
} // namespace ZHLN
