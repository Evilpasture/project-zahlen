// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include <array>
#include <threading/TaskSystem.hpp>

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

struct GpuCullingPolicy {
    static void Record(
        const FrameRecorder&                                     recorder,
        const ZHLN::Array<GroupRange>&                           groups,
        uint32_t                                                 drawCount,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> color_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> vel_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att
    ) noexcept {
        VkCommandBuffer cmd = recorder.cmd;
        auto&           ctx = recorder.ctx;

        struct FrustumPlanes {
            std::array<JPH::Vec4, 6> planes;
            uint32_t                 drawCount;
        } planes {};

        const auto& vp = ctx.unjittered_view_proj;
        JPH::Vec4   r0(vp(0, 0), vp(0, 1), vp(0, 2), vp(0, 3));
        JPH::Vec4   r1(vp(1, 0), vp(1, 1), vp(1, 2), vp(1, 3));
        JPH::Vec4   r2(vp(2, 0), vp(2, 1), vp(2, 2), vp(2, 3));
        JPH::Vec4   r3(vp(3, 0), vp(3, 1), vp(3, 2), vp(3, 3));

        auto NormalizePlane = [&](const JPH::Vec4& plane) {
            float len = JPH::Vec3(plane.GetX(), plane.GetY(), plane.GetZ()).Length();
            return plane / std::max(len, 1e-6f);
        };

        planes.planes[0] = NormalizePlane(r3 + r0);
        planes.planes[1] = NormalizePlane(r3 - r0);
        planes.planes[2] = NormalizePlane(r3 + r1);
        planes.planes[3] = NormalizePlane(r3 - r1);
        planes.planes[4] = NormalizePlane(r2);
        planes.planes[5] = NormalizePlane(r3 - r2);
        planes.drawCount = drawCount;

        ctx.cullingPass.Dispatch(cmd, ctx.cullingSets[], (drawCount + 63) / 64, 1, 1, planes);
        using enum Vk::BarrierStage;
        using enum Vk::BarrierAccess;

        Vk::BeginBarrier<Compute, ShaderWrite>(Vk::CommandBuffer<Vk::QueueType::Graphics> {cmd}).TransitionTo<Indirect, IndirectRead>();

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
                            .argumentBuffer = ctx.indirectCommandsBuffers->Handle(),
                            .offset         = Vk::DrawIndirectState::OffsetForIndex(group.start),
                            .drawCount      = group.count,
                        },
                        ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 0}
                    );
                }
            });
    }
};

struct CpuCullingPolicy {
    static void Record(
        const FrameRecorder& recorder,
        const ZHLN::Array<GroupRange>& /*groups*/,
        uint32_t                                                 drawCount,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> color_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> vel_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att
    ) noexcept {
        VkCommandBuffer cmd = recorder.cmd;
        auto&           ctx = recorder.ctx;

        const auto& colorFormats = ActiveGBuffer::array;

        Vk::DynamicPass(color_att.extent)
            .AddColor(color_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
            .AddColor(vel_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorVelocity)
            .AddColor(norm_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorNormalRoughness)
            .AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearDepthValue)
            .Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
            .Execute(cmd, [&]() {
                Vk::ParallelDrawDispatch(
                    cmd, Vk::SecondaryInheritance {.colorFormats = colorFormats, .depthFormat = VK_FORMAT_D32_SFLOAT},
                    {.width = color_att.extent.width, .height = color_att.extent.height}, // Explicit 3D -> 2D Truncation
                    drawCount, kParallelChunkSize,

                    TaskSystemSchedulerAdapter {},
                    [&]([[maybe_unused]] uint32_t chunkIdx) -> VkCommandBuffer {
                        uint32_t wIdx = TaskSystem::GetWorkerIndex();
                        if (wIdx >= ctx.workerCmds.size()) {
                            wIdx = (uint32_t) (ctx.workerCmds.size() - 1);
                        }
                        uint32_t localCmdIdx = ctx.workerCmds[wIdx].cmdCount[recorder.frameIndex].fetch_add(1, std::memory_order::relaxed);
                        return ctx.workerCmds[wIdx].pools[recorder.frameIndex][localCmdIdx];
                    },
                    [&](Vk::CommandEncoder& encoder, uint32_t i) {
                        const auto& drawCmd = ctx.drawQueue[i];
                        if (!IsVisibleIn(drawCmd.flags, RenderPassType::Main)) {
                            return;
                        }
                        if (!drawCmd.material->pipeline.Valid() || IsForwardOnly(drawCmd.instanceData.flags)) {
                            return;
                        }
                        const ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};
                        SubmitDrawInstanced(encoder, drawCmd, i, recorder.bindlessSet, pushConstants);
                    }
                );
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

    auto  mapped           = ctx.shadowIndirectBuffers->Map();
    auto* indirectCmdsBase = static_cast<VkDrawIndirectCommand*>(mapped.data);

    std::array<uint32_t, 8> passWriteOffsets {};
    passWriteOffsets[0] = 0; // We use a single batch offset for all 4 Cascades via Multiview
    for (uint32_t l = 0; l < 4; ++l) {
        passWriteOffsets[4 + l] = (4 + l) * kGpuCullingMaxInstances;
    }

    std::array<uint32_t, 8> passDrawCounts {};

    // 1. Pre-filter active shadow-casting point lights once to eliminate redundant loop overhead
    std::array<const GPULight*, 4> activeShadowLights {};
    uint32_t                       activeShadowLightCount = 0;
    for (const auto& light: ctx.mappedLights) {
        if (light.shadowLayer >= 0 && light.type == Point) {
            activeShadowLights[activeShadowLightCount++] = &light;
            if (activeShadowLightCount >= 4) {
                break;
            }
        }
    }

    for (uint32_t i = 0; i < ctx.drawQueue.size(); ++i) {
        const auto& drawCmd = ctx.drawQueue[i];

        if (!IsVisibleIn(drawCmd.flags, Shadow) || IsForwardOnly(drawCmd.instanceData.flags)) {
            continue;
        }

        uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;

        JPH::Vec3 meshPos = drawCmd.instanceData.world.GetTranslation();
        float     radius  = drawCmd.instanceData.cullRadius;

        // Directional Cascaded Shadow (Multiview processes all 4 layers from a single buffer slice)
        uint32_t writeIdx          = passWriteOffsets[0] + passDrawCounts[0];
        indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
        passDrawCounts[0]++;

        // 2. Iterate only over pre-filtered shadow-casting lights instead of the entire scene light list
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
        Profiler::ScopedGpuProfile<Stages::ShadowPass, FrameProfiler> timer(cmd, recorder.frameIndex, ctx.gpuProfiler);

        uint32_t csmDrawCount = passDrawCounts[0];
        if (csmDrawCount > 0) {
            Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> cascadeLayerImage = {
                .handle = ctx.graphResources.shadowMap.image.Handle(),
                .view   = ctx.graphResources.shadowMap.view.Get(),
                .extent = {.width = ctx.graphResources.shadowMap.extent.width, .height = ctx.graphResources.shadowMap.extent.height, .depth = 1},
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT
            };

            Vk::DynamicPass(cascadeLayerImage.extent)
                .ViewMask(0xF) // Broadcast to all 4 Cascades cleanly via Multiview
                .AddDepth(cascadeLayerImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kShadowClearDepth)
                .Execute(cmd, [&]() {
                    recorder.encoder.DrawIndirect(
                        {.pipeline       = ctx.shadowPipeline.Get(),
                         .layout         = ctx.shadowPipelineLayout.Get(),
                         .set            = recorder.bindlessSet,
                         .argumentBuffer = ctx.shadowIndirectBuffers->Handle(),
                         .offset         = Vk::DrawIndirectState::OffsetForIndex(passWriteOffsets[0]),
                         .drawCount      = csmDrawCount},
                        ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 1}, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
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
            if (light.shadowLayer < 0 || light.type != Point) {
                continue;
            }

            uint32_t slotIdx   = 4 + light.shadowLayer;
            uint32_t drawCount = passDrawCounts[slotIdx];
            if (drawCount == 0) {
                continue;
            }

            Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> subViewImage = {
                .handle = ctx.graphResources.shadowAtlas.image.Handle(),
                .view   = ctx.punctualShadowViews[light.shadowLayer].Get(),
                .extent = {.width = 1024, .height = 1024, .depth = {}},
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT
            };

            ExecutePunctualPass(subViewImage, [&]() {
                const struct PunctualPush {
                    uint32_t lightIdx;
                } pc = {l_idx};

                recorder.encoder.DrawIndirect(
                    {
                        .pipeline       = ctx.punctualShadowPipeline.Get(),
                        .layout         = ctx.punctualShadowPipelineLayout.Get(),
                        .set            = recorder.bindlessSet,
                        .argumentBuffer = ctx.shadowIndirectBuffers->Handle(),
                        .offset         = Vk::DrawIndirectState::OffsetForIndex(passWriteOffsets[slotIdx]),
                        .drawCount      = drawCount,
                    },
                    pc, VK_SHADER_STAGE_VERTEX_BIT
                );
            });
        }
    }
}

void MainPass::Execute(
    const FrameRecorder&                                                                               recorder,
    SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> in
) const noexcept {
    auto  cmd = recorder.cmd;
    auto& ctx = recorder.ctx;

    Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, recorder.frameIndex, ctx.gpuProfiler);

    const auto drawCount = static_cast<uint32_t>(ctx.drawQueue.size());
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
        const auto&       drawCmd = ctx.drawQueue[i];
        const auto* const drawMat = drawCmd.material;

        if (IsForwardOnly(drawCmd.instanceData.flags) || !drawMat->pipeline.Valid()) {
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

    const bool useGpuCulling = [&]() {
        return ctx.cullingPass.pipeline.Valid() && ctx.indirectCommandsBuffers->Valid() && (drawCount <= kGpuCullingMaxInstances);
    }();

    if (useGpuCulling) {
        ExecutePass<GpuCullingPolicy>(recorder, groups, drawCount, in.sceneColor, in.velocity, in.normRough, in.depth);
    } else {
        ExecutePass<CpuCullingPolicy>(recorder, groups, drawCount, in.sceneColor, in.velocity, in.normRough, in.depth);
    }
}

void ForwardPass::Execute(
    const FrameRecorder&                                     recorder,
    Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> litColor,
    Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth
) const noexcept {
    VkCommandBuffer cmd = recorder.cmd;
    const auto&     ctx = recorder.ctx;

    Vk::DynamicPass(litColor.extent)
        .AddColor(litColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            // Draw standard forward transparent geometry...

            // --- DRAW 65,536 GPU SNOWFLAKE BILLBOARDS (NATIVE FRAMEWORK) ---
            if (ctx.particleRenderPipeline.Valid()) {
                struct ParticleRenderPushConstants {
                    VkDeviceAddress particleBufferAddr;
                } pc = {.particleBufferAddr = ctx.BufferAddress(ctx.particleBuffer.Handle())};

                recorder.encoder.DrawInstanced(
                    {.pipeline      = ctx.particleRenderPipeline.Get(),
                     .layout        = ctx.particleRenderLayout.Get(),
                     .set           = recorder.bindlessSet,
                     .vertexCount   = 6, // Quad (2 triangles = 6 vertices)
                     .instanceCount = RenderContext::Impl::kGpuParticleCount,
                     .firstVertex   = 0,
                     .firstInstance = 0},
                    pc, VK_SHADER_STAGE_VERTEX_BIT
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

    Profiler::ScopedGpuProfile<Stages::BlitPass, FrameProfiler> timer(cmd, recorder.frameIndex, ctx.gpuProfiler);

    struct BlitPushConstants {
        float vignetteIntensity;
        float vignettePower;
        int   fullBright;
    } pc = {.vignetteIntensity = ctx.giSettings.vignetteIntensity, .vignettePower = ctx.giSettings.vignettePower, .fullBright = fullBright};

    if (ctx.blitPass.pipeline.Valid()) {
        Vk::DynamicPass(inColor.extent).AddColor(swapchainTarget, VK_ATTACHMENT_LOAD_OP_DONT_CARE).Execute(cmd, [&]() {
            ctx.blitPass.Execute(cmd, pc);
            if (!ctx.uiDrawQueue.empty()) {
                UIObjectConstants uipc {};
                uipc.orthoMatrix = Math::CreateOrthoMatrix(inColor.extent.width, inColor.extent.height);

                VkRect2D defaultScissor = {.offset = {.x = 0, .y = 0}, .extent = {.width = inColor.extent.width, .height = inColor.extent.height}};

                for (const auto& draw: ctx.uiDrawQueue) {
                    uipc.albedoIdx   = draw.fontIndex;
                    uipc.posAddress  = draw.posMesh->vboAddress;
                    uipc.attrAddress = draw.attrMesh->vboAddress;

                    Vk::ScopedScissor scissorGuard(
                        cmd, Vk::ScopedScissor::ScissorDesc {
                                 .target   = draw.useScissor ?
                                                 VkRect2D {
                                                     .offset = {.x = draw.scissorRect.x, .y = draw.scissorRect.y},
                                                     .extent = {.width = draw.scissorRect.width, .height = draw.scissorRect.height}
                                                 } :
                                                 defaultScissor,
                                 .fallback = defaultScissor
                             }
                    );

                    recorder.encoder.DrawInstanced(
                        {.pipeline    = ctx.uiPipeline.Get(),
                         .layout      = ctx.uiPipelineLayout.Get(),
                         .set         = recorder.bindlessSet,
                         .vertexCount = draw.posMesh->vertexCount},
                        uipc
                    );
                }
                ctx.uiDrawQueue.clear();
            }
            if (!ctx.window.IsTTY()) {
                ImGui::Render();
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            }
        });
    }
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swapchainTarget.handle);
}

} // namespace Passes
} // namespace ZHLN
