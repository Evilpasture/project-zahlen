// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "imgui.h"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>

namespace ZHLN {

namespace {
struct TaskSystemSchedulerAdapter {
    void ParallelFor(uint32_t count, uint32_t chunkSize, auto&& func) const {
        TaskSystem::ParallelFor(count, chunkSize, std::forward<decltype(func)>(func));
    }
};
enum class RenderPassType : uint8_t { Main, Shadow };

[[nodiscard]] PackedRGBA8 ImGuiColor(ImU32 color) noexcept {
    return PackedRGBA8 {color};
}

[[nodiscard]] VertexPosition ImGuiPosition(const ImDrawVert& v) noexcept {
    return VertexPosition {.position = {v.pos.x, v.pos.y, 0.0f}};
}

[[nodiscard]] VertexAttributes ImGuiAttributes(const ImDrawVert& v) noexcept {
    return VertexAttributes {.normal = {}, .tangent = {}, .uv = Math::PackUV(v.uv.x, v.uv.y), .color = ImGuiColor(v.col)};
}

void AppendImGuiBatches(RenderContext::Impl& ctx, uint32_t frameIndex, VkExtent2D extent) noexcept {
    if (ctx.window.IsTTY() || ImGui::GetCurrentContext() == nullptr || !ctx.imguiFrameOpen) {
        return;
    }

    ImGui::Render();
    ctx.imguiFrameOpen = false;
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->TotalIdxCount <= 0 || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }

    auto&  vbo         = ctx.frames.uiVbos[frameIndex];
    size_t maxVertices = vbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));
    if (maxVertices == 0) {
        return;
    }

    uint32_t vertexCursor = 0;
    for (const auto& batch: ctx.queues.uiBatches) {
        vertexCursor = std::max(vertexCursor, batch.vertexStart + batch.vertexCount);
    }
    if (vertexCursor >= maxVertices) {
        return;
    }

    auto  mappedRegion = vbo.Map();
    auto* basePosPtr   = static_cast<VertexPosition*>(mappedRegion.data);
    auto* baseAttrPtr  = reinterpret_cast<VertexAttributes*>(basePosPtr + maxVertices);

    const ImVec2 clipOff   = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (const ImDrawList* list: drawData->CmdLists) {
        for (const ImDrawCmd& drawCmd: list->CmdBuffer) {
            if (drawCmd.UserCallback != nullptr || drawCmd.ElemCount == 0) {
                continue;
            }

            if (drawCmd.ElemCount > maxVertices - vertexCursor) {
                return;
            }

            const uint32_t firstVertex = vertexCursor;
            for (uint32_t i = 0; i < drawCmd.ElemCount; ++i) {
                const ImDrawIdx idx       = list->IdxBuffer[drawCmd.IdxOffset + i];
                const ImDrawVert& v       = list->VtxBuffer[drawCmd.VtxOffset + idx];
                basePosPtr[vertexCursor]  = ImGuiPosition(v);
                baseAttrPtr[vertexCursor] = ImGuiAttributes(v);
                ++vertexCursor;
            }

            const uint32_t emitted = vertexCursor - firstVertex;
            if (emitted == 0) {
                return;
            }

            ImVec2 clipMin {(drawCmd.ClipRect.x - clipOff.x) * clipScale.x, (drawCmd.ClipRect.y - clipOff.y) * clipScale.y};
            ImVec2 clipMax {(drawCmd.ClipRect.z - clipOff.x) * clipScale.x, (drawCmd.ClipRect.w - clipOff.y) * clipScale.y};
            clipMin.x = std::clamp(clipMin.x, 0.0f, static_cast<float>(extent.width));
            clipMin.y = std::clamp(clipMin.y, 0.0f, static_cast<float>(extent.height));
            clipMax.x = std::clamp(clipMax.x, 0.0f, static_cast<float>(extent.width));
            clipMax.y = std::clamp(clipMax.y, 0.0f, static_cast<float>(extent.height));
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
                continue;
            }

            ctx.queues.uiBatches.push_back(
                UIBatch {.texture              = TextureHandle::Invalid,
                         .bindlessTextureIndex = static_cast<uint32_t>(static_cast<uintptr_t>(drawCmd.GetTexID())),
                         .vertexStart          = firstVertex,
                         .vertexCount          = emitted,
                         .useScissor           = true,
                         .isSDF                = false,
                         .useTextureColor      = true,
                         .scissorRect          = {.x      = static_cast<int32_t>(clipMin.x),
                                                  .y      = static_cast<int32_t>(clipMin.y),
                                                  .width  = static_cast<uint32_t>(clipMax.x - clipMin.x),
                                                  .height = static_cast<uint32_t>(clipMax.y - clipMin.y)}}
            );

            if (vertexCursor >= maxVertices) {
                return;
            }
        }
    }
}

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

/// VK_EXT_mesh_shader: a draw takes the meshlet path only when the material
/// carries a mesh pipeline, the instance carries meshlet streams and no
/// pipeline override (CSG stencil passes) is in play.
[[nodiscard]] inline bool UseMeshPath(const DrawCommand& drawCmd, VkPipeline pipelineOverride) noexcept {
    return pipelineOverride == VK_NULL_HANDLE && drawCmd.material != nullptr && drawCmd.material->HasMeshPipeline() && drawCmd.instanceData.meshletCount > 0 &&
           !Diag::DisableMeshShading();
}

/// Number of task workgroups needed to screen every meshlet of an instance;
/// each workgroup evaluates kMeshletsPerTaskGroup clusters (basic_task.slang).
[[nodiscard]] inline constexpr uint32_t TaskGroupCount(uint32_t meshletCount) noexcept {
    return (meshletCount + kMeshletsPerTaskGroup - 1) / kMeshletsPerTaskGroup;
}

template <typename T>
inline void SubmitDrawInstanced(
    Vk::CommandEncoder& encoder,
    const DrawCommand&  drawCmd,
    uint32_t            instanceIdx,
    const T&            pushConstants,
    VkPipeline          pipelineOverride = VK_NULL_HANDLE,
    VkPipelineLayout    layoutOverride   = VK_NULL_HANDLE,
    VkShaderStageFlags  stages           = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
) noexcept {
    const auto* nativeMat = drawCmd.material;
    auto* const layout    = (layoutOverride != VK_NULL_HANDLE) ? layoutOverride : nativeMat->layout;

    // --- VK_EXT_mesh_shader path -------------------------------------------
    // The task shader reads the instance id out of push data (exactly like the
    // vertex shader does), performs per-cluster frustum + normal-cone culling
    // and amplifies into one mesh workgroup per surviving meshlet. There is no
    // firstInstance to encode here, which is precisely why the mesh path runs
    // through this per-draw submission rather than the indirect one.
    if (UseMeshPath(drawCmd, pipelineOverride)) {
        encoder.DrawMeshTasks(
            {.pipeline    = nativeMat->meshPipeline.Get(),
             .layout      = layout,
             .heap        = true,
             .groupCountX = TaskGroupCount(drawCmd.instanceData.meshletCount),
             .groupCountY = 1,
             .groupCountZ = 1},
            pushConstants
        );
        return;
    }

    auto* const pipeline = (pipelineOverride != VK_NULL_HANDLE) ? pipelineOverride : nativeMat->pipeline.Get();

    const uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;

    // VK_EXT_descriptor_heap: heaps are bound on the command buffer; per-draw
    // data travels through push data (offset 0).
    encoder.DrawInstanced(
        {.pipeline = pipeline, .layout = layout, .heap = true, .vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = instanceIdx},
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
        Vk::ClearStencilAttachment(cmd, {.width = extent.width, .height = extent.height});

        for (const auto& cutter: csgCmd.cutters) {
            const ObjectConstants push = {.instanceId = cutter.instanceIdx, .isShadowPass = 0};
            SubmitDrawInstanced(recorder.encoder, cutter.draw, cutter.instanceIdx, push, ctx.csgWritePipeline.Get(), ctx.csgPipelineLayout);
        }

        VkPipeline activePipeline = ctx.csgDifferencePipeline.Get();
        if (!csgCmd.cutters.empty() && csgCmd.cutters[0].operation == CSGOperation::Intersection) {
            activePipeline = ctx.csgIntersectionPipeline.Get();
        }

        const ObjectConstants push = {.instanceId = csgCmd.eyeInstanceIdx, .isShadowPass = 0};
        SubmitDrawInstanced(recorder.encoder, csgCmd.eyeDraw, csgCmd.eyeInstanceIdx, push, activePipeline, ctx.csgPipelineLayout);
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
             .layout        = ctx.meshParticleRenderLayout,
             .heap          = true,
             .vertexCount   = drawVertexCount,
             .instanceCount = emitter.maxParticles,
             .firstVertex   = 0,
             .firstInstance = 0},
            rpc
        );
    }
}

void Draw3DParticleShadows(const FrameRecorder& recorder) noexcept {
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
            .cascadeIndex       = 0 // Legacy padding slot; the shader selects the cascade from ViewIndex.
        };
        std::memcpy(rpc.baseColorFactor, gpuMat->baseColorFactor, sizeof(float) * 4);

        uint32_t drawVertexCount = (iboMesh != nullptr) ? gpuMesh->indexCount : gpuMesh->vertexCount;

        recorder.encoder.DrawInstanced(
            {.pipeline      = ctx.meshParticleShadowPipeline.Get(),
             .layout        = ctx.meshParticleRenderLayout,
             .heap          = true,
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
        Vk::BufferBarrier(
            cmd, ctx.frames.secondPassCountBuffers[recorder.frameIndex].Handle(),
            Vk::BarrierStage::Compute | Vk::BarrierStage::Indirect, Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::IndirectRead,
            Vk::BarrierStage::Clear, Vk::BarrierAccess::TransferWrite
        );

        Vk::FillBuffer(cmd, ctx.frames.secondPassCountBuffers[recorder.frameIndex], 0, 0u);

        Vk::BufferBarrier(
            cmd, ctx.frames.secondPassCountBuffers[recorder.frameIndex].Handle(),
            Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite,
            Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::ShaderRead
        );

        // 2. Dispatch Culling Pass 1 (Frustum + Last Frame Hi-Z)
        struct CullingConstants {
            JPH::Mat44           viewProj;
            std::array<float, 2> hizScreenSize;
            uint32_t             maxHiZMipLevel;
            uint32_t             drawCount;
            uint32_t             passIndex;
        } pc {};

        pc.viewProj         = ctx.unjittered_view_proj;
        pc.hizScreenSize[0] = static_cast<float>(color_att.extent.width);
        pc.hizScreenSize[1] = static_cast<float>(color_att.extent.height);
        const uint32_t hizMips = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);
        pc.maxHiZMipLevel   = hizMips > 0 ? hizMips - 1 : 0;
        pc.drawCount        = drawCount;
        pc.passIndex        = 0; // PASS 1

        ctx.cullingPass.DispatchHeapIndexedThreads(ctx.ctx, cmd, 0 * 2 + recorder.frameIndex, drawCount, 1, 1, pc);

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
                // The culling dispatch above is a heap pass using push data at
                // offsets 0/176, which invalidated the push-data state: re-bind
                // the heaps (primary segments only — inherited secondaries keep
                // theirs) and re-push the frame address block for the
                // heap-based geometry draws.
                recorder.EnsureHeapState(cmd);

                for (const auto& group: groups) {
                    if (!group.material->pipeline.Valid()) {
                        continue;
                    }
                    recorder.encoder.DrawIndirect(
                        {
                            .pipeline       = group.material->pipeline.Get(),
                            .layout         = group.material->layout,
                            .heap           = true,
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

        Vk::BufferBarrier(
            cmd, ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex].Handle(),
            Vk::BarrierStage::Compute | Vk::BarrierStage::Indirect, Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::IndirectRead,
            Vk::BarrierStage::Clear, Vk::BarrierAccess::TransferWrite
        );

        Vk::FillBuffer(cmd, ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex], 0, 0u);

        Vk::BufferBarrier(
            cmd, ctx.frames.indirectCommandsBuffersPass2[recorder.frameIndex].Handle(),
            Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferWrite,
            Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::ShaderRead
        );

        // 2. Dispatch Culling Pass 2 (Current Frame Hi-Z Re-test)

        const uint32_t hizMips2 = std::min(ctx.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);
        RenderContext::Impl::CullingConstants pc {
            .viewProj       = ctx.unjittered_view_proj,
            .hizScreenSize  = {static_cast<float>(color_att.extent.width), static_cast<float>(color_att.extent.height)},
            .maxHiZMipLevel = hizMips2 > 0 ? hizMips2 - 1 : 0,
            .drawCount      = drawCount,
            .passIndex      = 1,
        };
        ctx.cullingPass.DispatchHeapIndexedThreads(ctx.ctx, cmd, 1 * 2 + recorder.frameIndex, drawCount, 1, 1, pc);

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
                // Re-establish heap + push-data state after the culling dispatch
                // (push data was updated for the dispatch).
                recorder.EnsureHeapState(cmd);

                for (const auto& group: groups) {
                    if (!group.material->pipeline.Valid()) {
                        continue;
                    }
                    recorder.encoder.DrawIndirect(
                        {
                            .pipeline       = group.material->pipeline.Get(),
                            .layout         = group.material->layout,
                            .heap           = true,
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
                // Heap + push-data state for the parallel geometry secondaries.
                // The secondaries inherit the heap binding and each pushes the
                // per-frame device-address block once before its draws.
                ctx.BindHeapsAndPushFrame(cmd);
                const auto frameAddresses = ctx.FrameHeapAddresses();
                const auto samplerBind    = ctx.heapManager.GetSamplerHeapBindInfo();
                const auto resourceBind   = ctx.heapManager.GetResourceHeapBindInfo();

                Vk::ParallelDrawDispatch(
                    cmd,
                    Vk::SecondaryInheritance {
                        .colorFormats           = colorFormats,
                        .depthFormat            = VK_FORMAT_D32_SFLOAT_S8_UINT,
                        .samplerHeapBindInfo    = &samplerBind,
                        .resourceHeapBindInfo   = &resourceBind,
                        .context                = &ctx.ctx,
                        .pushDataFrameOffsets   = ctx.heapPushDataLayout.frameAddressOffsets,
                        .pushDataFrameAddresses = std::span<const VkDeviceAddress> {frameAddresses.data(), frameAddresses.size()},
                    },
                    {.width = color_att.extent.width, .height = color_att.extent.height}, drawCount, kParallelChunkSize, TaskSystemSchedulerAdapter {},
                    [&](uint32_t /*chunkIdx*/) -> VkCommandBuffer {
                        uint32_t wIdx = TaskSystem::GetWorkerIndex();
                        if (wIdx >= ctx.workerCmds.size()) {
                            wIdx = static_cast<uint32_t>(ctx.workerCmds.size() - 1);
                        }
                        uint32_t localCmdIdx = ctx.workerCmds[wIdx].cmdCount[recorder.frameIndex].fetch_add(1, std::memory_order::relaxed);
                        return ctx.workerCmds[wIdx].pools[recorder.frameIndex][localCmdIdx];
                    },
                    [&](Vk::CommandEncoder& encoder, uint32_t i) {
                        const auto& drawCmd = ctx.queues.drawQueue[i];
                        if (!IsVisibleIn(drawCmd.flags, RenderPassType::Main) || (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None ||
                            !drawCmd.material->pipeline.Valid() || IsForwardOnly(drawCmd.instanceData.flags)) {
                            return;
                        }
                        SubmitDrawInstanced(encoder, drawCmd, i, ObjectConstants {.instanceId = i, .isShadowPass = 0});
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
        auto&           ctx = recorder.ctx;
        Vk::DynamicPass(color_att.extent)
            .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .Execute(cmd, [&]() {
                ctx.BindHeapsAndPushFrame(cmd);
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

    // VK_EXT_descriptor_heap: the shadow pass runs entirely on heap pipelines.
    // It records into either the primary (serial fallback) or a secondary
    // command buffer (parallel recorder). In the secondary case the heaps are
    // inherited from the primary and the frame addresses were re-pushed by
    // the recorder, so only the primary path self-binds.
    recorder.EnsureHeapState(cmd);

    std::array<Frustum, RenderContext::Impl::NUM_CASCADES> cascadeFrustums {};
    for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
        cascadeFrustums[c].Update(ctx.currentUniforms.lightSpaceMatrices[c]);
    }

    auto  mapped           = ctx.frames.shadowIndirectBuffers->Map();
    auto* indirectCmdsBase = static_cast<VkDrawIndirectCommand*>(mapped.data);

    std::array<uint32_t, 8> passWriteOffsets {};
    // Slot 0: the multiview cascade draw list. All four cascades render from
    // ONE list now -- a mesh is listed once if ANY cascade intersects it, and
    // each view (cascade) clips it in the vertex stage against its own
    // light-space matrix. Slots 4..7 stay per-punctual-light.
    passWriteOffsets[0] = 0;
    for (uint32_t l = 0; l < RenderContext::Impl::MAX_PUNCTUAL_LIGHTS; ++l) {
        passWriteOffsets[4 + l] = (4 + l) * kGpuCullingMaxInstances;
    }

    std::array<uint32_t, 8> passDrawCounts {};

    std::array<const Light*, RenderContext::Impl::MAX_PUNCTUAL_LIGHTS> activeShadowLights {};
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

        // Cascade Culling: emit the mesh once if ANY cascade geometrically
        // intersects it. The multiview pass re-tests per view in clip space,
        // so this only decides which meshes enter the shared cascade list.
        bool inAnyCascade = false;
        for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
            if (cascadeFrustums[c].IsSphereVisible(meshPos, radius)) {
                inAnyCascade = true;
                break;
            }
        }
        if (inAnyCascade) {
            uint32_t writeIdx          = passWriteOffsets[0] + passDrawCounts[0];
            indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
            passDrawCounts[0]++;
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
        bool hasMeshParticles = !ctx.queues.meshParticleQueue.empty();

        const bool useMeshShadowPath =
            ctx.MeshShadingActive() && ctx.MultiviewMeshShadingEnabled() && ctx.shadowMeshPipeline.Valid();

        uint32_t csmDrawCount = passDrawCounts[0];

        // ONE layered render pass for all four cascades: Vulkan multiview
        // (viewMask 0x0F) fans every draw out to the shadow map's four array
        // layers and hands ViewIndex to the shaders, replacing four sequential
        // Vk::DynamicPass instances (four render-target switches, four clears,
        // four begin/end cycles) with a single one.
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> shadowMapArrayImage = {
            .handle = ctx.graphResources.shadowMap.image.Handle(),
            .view   = ctx.graphResources.shadowMap.view.Get(),
            .extent = {.width = ctx.graphResources.shadowMap.extent.width, .height = ctx.graphResources.shadowMap.extent.height, .depth = 1},
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT
        };

        Vk::DynamicPass(shadowMapArrayImage.extent)
            .ViewMask(kCascadeViewMask)
            .AddDepth(shadowMapArrayImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kShadowClearDepth)
            .Execute(cmd, [&]() {
                // VK_EXT_mesh_shader: VkDrawMeshTasksIndirectCommandEXT has
                // no firstInstance, so the instance id can no longer ride
                // along in the indirect record. The cascade visibility list
                // was just written to a host-visible buffer above, so the
                // mesh path simply replays it as direct dispatches -- the
                // per-cluster culling that matters now happens in the task
                // shader against lightSpaceMatrices[ViewIndex].
                const bool useMeshShadows = useMeshShadowPath && csmDrawCount > 0;

                if (useMeshShadows) {
                    for (uint32_t d = 0; d < csmDrawCount; ++d) {
                        const uint32_t instanceIdx = indirectCmdsBase[passWriteOffsets[0] + d].firstInstance;
                        if (instanceIdx >= ctx.queues.drawQueue.size()) {
                            continue;
                        }
                        const auto& shadowDraw = ctx.queues.drawQueue[instanceIdx];
                        if (shadowDraw.instanceData.meshletCount == 0) {
                            // Skinned / non-meshletized geometry: one vertex draw.
                            recorder.encoder.DrawInstanced(
                                {.pipeline      = ctx.shadowPipeline.Get(),
                                 .layout        = ctx.shadowPipelineLayout,
                                 .heap          = true,
                                 .vertexCount   = indirectCmdsBase[passWriteOffsets[0] + d].vertexCount,
                                 .instanceCount = 1,
                                 .firstVertex   = 0,
                                 .firstInstance = instanceIdx},
                                ObjectConstants {.instanceId = instanceIdx, .isShadowPass = 1},
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                            );
                            continue;
                        }

                        recorder.encoder.DrawMeshTasks(
                            {.pipeline    = ctx.shadowMeshPipeline.Get(),
                             .layout      = ctx.shadowPipelineLayout,
                             .heap        = true,
                             .groupCountX = TaskGroupCount(shadowDraw.instanceData.meshletCount),
                             .groupCountY = 1,
                             .groupCountZ = 1},
                            ObjectConstants {.instanceId = instanceIdx, .isShadowPass = 1}
                        );
                    }
                } else if (csmDrawCount > 0) {
                    recorder.encoder.DrawIndirect(
                        {.pipeline       = ctx.shadowPipeline.Get(),
                         .layout         = ctx.shadowPipelineLayout,
                         .heap           = true,
                         .argumentBuffer = ctx.frames.shadowIndirectBuffers->Handle(),
                         .offset         = Vk::DrawIndirectState::OffsetForIndex(passWriteOffsets[0]),
                         .drawCount      = csmDrawCount},
                        ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 1}, // Cascade index comes from ViewIndex.
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
                }

                if (hasMeshParticles) {
                    Draw3DParticleShadows(recorder);
                }
            });
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
                            .layout         = ctx.punctualShadowPipelineLayout,
                            .heap           = true,
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
    auto       cmd       = recorder.cmd;
    auto&      ctx       = recorder.ctx;
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

    // VK_EXT_mesh_shader: the two-phase GPU culling path drives the geometry
    // through vkCmdDrawIndirect, whose VkDrawIndirectCommand::firstInstance
    // carries the instance id. The mesh equivalent
    // (VkDrawMeshTasksIndirectCommandEXT) has no such field, so while mesh
    // shading is active the passes take the per-draw recording policy and the
    // culling work moves into the task shader (per-cluster frustum + normal
    // cone) instead of the instance-level culling compute pass.
    const bool useGpuCulling = ctx.cullingPass.pipeline.Valid() && ctx.frames.indirectCommandsBuffers->Valid() && (drawCount <= kGpuCullingMaxInstances) &&
                               !Diag::DisableGpuCulling() && !ctx.MeshShadingActive();
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
    auto&      ctx       = recorder.ctx;
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

    // See MainPass1: mesh shading and the indirect culling path are mutually
    // exclusive because the mesh indirect command has no firstInstance field.
    const bool useGpuCulling = ctx.cullingPass.pipeline.Valid() && ctx.frames.indirectCommandsBuffers->Valid() && (drawCount <= kGpuCullingMaxInstances) &&
                               !Diag::DisableGpuCulling() && !ctx.MeshShadingActive();
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

    ctx.BindHeapsAndPushFrame(cmd);

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
                    recorder.encoder, drawCmd, static_cast<uint32_t>(i), push, drawCmd.prePassMaterial->pipeline.Get(), drawCmd.prePassMaterial->layout
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

    ctx.BindHeapsAndPushFrame(cmd);

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

                SubmitDrawInstanced(recorder.encoder, drawCmd, static_cast<uint32_t>(i), push);
            }

            if (ctx.particleRenderPipeline.Valid() && !ctx.queues.particleEmittersQueue.empty()) {
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
                         .layout        = ctx.particleRenderLayout,
                         .heap          = true,
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
                     .layout        = ctx.linePipelineLayout,
                     .heap          = true,
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

    struct BlitPushConstants {
        float vignetteIntensity;
        float vignettePower;
        int   fullBright;
    } pc = {.vignetteIntensity = ctx.settings.post.vignetteIntensity, .vignettePower = ctx.settings.post.vignettePower, .fullBright = fullBright};

    if (ctx.blitPass.pipeline.Valid()) {
        Vk::DynamicPass(inColor.extent).AddColor(swapchainTarget, VK_ATTACHMENT_LOAD_OP_DONT_CARE).Execute(cmd, [&]() {
            ctx.blitPass.ExecuteHeap(ctx.ctx, cmd, pc, recorder.frameIndex);
            AppendImGuiBatches(ctx, recorder.frameIndex, {.width = inColor.extent.width, .height = inColor.extent.height});

            if (!ctx.queues.uiBatches.empty()) {
                // blitPass is a legacy descriptor-set + push-constant pass; the
                // UI batch pipeline is heap-based, so re-establish heap state.
                ctx.BindHeapsAndPushFrame(cmd);
                UIObjectConstants uipc {};
                uipc.orthoMatrix = Math::CreateOrthoMatrix(inColor.extent.width, inColor.extent.height);

                VkRect2D defaultScissor = {.offset = {.x = 0, .y = 0}, .extent = {.width = inColor.extent.width, .height = inColor.extent.height}};

                auto   baseVboAddress = ctx.frames.uiVboAddresses[recorder.frameIndex];
                size_t maxVertices    = ctx.frames.uiVbos[recorder.frameIndex].Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));

                for (const auto& batch: ctx.queues.uiBatches) {
                    uipc.albedoIdx        = batch.bindlessTextureIndex != 0 ? batch.bindlessTextureIndex : ctx.textureManager.GetBindlessIndex(batch.texture);
                    uipc.isSDF            = batch.isSDF ? 1 : 0;
                    uipc.useTextureColor  = batch.useTextureColor ? 1 : 0;
                    uipc.posAddress       = baseVboAddress + (batch.vertexStart * sizeof(VertexPosition));
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
                         .layout        = ctx.uiPipelineLayout,
                         .heap          = true,
                         .vertexCount   = batch.vertexCount,
                         .instanceCount = 1,
                         .firstVertex   = 0,
                         .firstInstance = 0},
                        uipc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
                }
            }
        });
    }
    if (ctx.presentation.swapchain.Valid()) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swapchainTarget.handle);
    }
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

    ctx.BindHeapsAndPushFrame(cmd);

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
                SubmitDrawInstanced(recorder.encoder, drawCmd, static_cast<uint32_t>(i), push);
            }
        });
}

} // namespace Passes
} // namespace ZHLN
