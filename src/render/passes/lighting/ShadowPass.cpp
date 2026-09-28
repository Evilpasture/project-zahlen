// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ShadowPass.hpp"
#include "features/ShadowRenderer.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Math3D.hpp>

namespace ZHLN::Passes {

namespace {

struct PunctualPush {
    uint32_t lightIndex;
};
static_assert(GpuAbi::ScenePassPayload<PunctualPush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

// One slot per shadow-casting view: slot 0 is the cascade pass, slots 4..7 the
// punctual lights'.
constexpr uint32_t kSlotCount        = 8;
constexpr uint32_t kFirstPunctualSlot = 4;

} // namespace

void ShadowPass::operator()(VkCommandBuffer cmd) const noexcept {
    using enum LightType;

    PassContext    passCtx(cmd, impl, impl.InheritsHeaps());
    auto&          ctx        = passCtx.ctx;
    const uint32_t frameIndex = ctx.presenter.frameIndex;

    passCtx.EnsureHeapState();

    std::array<Frustum, RenderContext::Impl::NUM_CASCADES> cascadeFrustums {};
    for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
        cascadeFrustums[c].Update(ctx.currentUniforms.lightSpaceMatrices[c]);
    }

    auto  mapped           = ctx.shadows.IndirectCommands(frameIndex).Map();
    auto* indirectCmdsBase = static_cast<VkDrawIndirectCommand*>(mapped.data);

    std::array<uint32_t, kSlotCount> passWriteOffsets {};
    passWriteOffsets[0] = 0;
    for (uint32_t l = 0; l < RenderContext::Impl::MAX_PUNCTUAL_LIGHTS; ++l) {
        passWriteOffsets[kFirstPunctualSlot + l] = (kFirstPunctualSlot + l) * kGpuCullingMaxInstances;
    }

    std::array<uint32_t, kSlotCount> passDrawCounts {};

    std::array<const Light*, RenderContext::Impl::MAX_PUNCTUAL_LIGHTS> activeShadowLights {};
    uint32_t                                                           activeShadowLightCount = 0;
    for (const auto& light: ctx.mappedLights) {
        if (light.shadowLayer >= 0 && light.type == Point) {
            activeShadowLights[activeShadowLightCount++] = &light;
            if (activeShadowLightCount >= RenderContext::Impl::MAX_PUNCTUAL_LIGHTS) {
                break;
            }
        }
    }

    for (uint32_t i = 0; i < ctx.queues.Draws().size(); ++i) {
        const auto& drawCmd = ctx.queues.Draws()[i];

        if (!IsVisibleIn(drawCmd.flags, RenderPassType::Shadow) || IsForwardOnly(drawCmd.instanceData.flags)) {
            continue;
        }

        const uint32_t vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;
        const JPH::Vec3 meshPos    = drawCmd.instanceData.world.GetTranslation();
        const float     radius     = drawCmd.instanceData.cullRadius;

        bool inAnyCascade = false;
        for (uint32_t c = 0; c < RenderContext::Impl::NUM_CASCADES; ++c) {
            if (cascadeFrustums[c].IsSphereVisible(meshPos, radius)) {
                inAnyCascade = true;
                break;
            }
        }
        if (inAnyCascade) {
            const uint32_t writeIdx = passWriteOffsets[0] + passDrawCounts[0];
            indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
            passDrawCounts[0]++;
        }

        for (uint32_t l = 0; l < activeShadowLightCount; ++l) {
            const auto* light   = activeShadowLights[l];
            const uint32_t slotIdx = kFirstPunctualSlot + light->shadowLayer;
            if (slotIdx >= kSlotCount) {
                continue;
            }

            const JPH::Vec3 lightPos(light->position[0], light->position[1], light->position[2]);
            const float     distToLightSq = (meshPos - lightPos).LengthSq();
            const float     maxRange      = light->range + radius;

            if (distToLightSq <= (maxRange * maxRange)) {
                const uint32_t pWriteIdx = passWriteOffsets[slotIdx] + passDrawCounts[slotIdx];
                indirectCmdsBase[pWriteIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
                passDrawCounts[slotIdx]++;
            }
        }
    }

    {
        const bool hasMeshParticles = !ctx.queues.MeshParticleEmitters().empty();

        const bool multiviewMesh = ctx.ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>([](const VkPhysicalDeviceMeshShaderFeaturesEXT& f) -> bool {
            return f.multiviewMeshShader == VK_TRUE;
        });
        const bool useMeshShadowPath = ctx.MeshShadingActive() && multiviewMesh && ctx.shadows.CascadeMeshPipeline() != VK_NULL_HANDLE;

        const uint32_t csmDrawCount = passDrawCounts[0];

        const Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> shadowMapArrayImage = {
            .handle = ctx.graphResources.shadowMap.image.Handle(),
            .view   = ctx.graphResources.shadowMap.view.Get(),
            .extent = {.width = ctx.graphResources.shadowMap.extent.width, .height = ctx.graphResources.shadowMap.extent.height, .depth = 1},
            .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            .format = VK_FORMAT_D32_SFLOAT,
            .info   = &ctx.graphResources.shadowMap.view.Info()
        };

        Vk::DynamicPass(shadowMapArrayImage.extent)
            .ViewMask(ShadowRenderer::kCascadeViewMask)
            .AddDepth(shadowMapArrayImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ShadowRenderer::kShadowClearDepth)
            .Execute(cmd, [&]() {
                const bool useMeshShadows = useMeshShadowPath && csmDrawCount > 0;

                if (useMeshShadows) {
                    for (uint32_t d = 0; d < csmDrawCount; ++d) {
                        const uint32_t instanceIdx = indirectCmdsBase[passWriteOffsets[0] + d].firstInstance;
                        if (instanceIdx >= ctx.queues.Draws().size()) {
                            continue;
                        }
                        const auto& shadowDraw = ctx.queues.Draws()[instanceIdx];
                        if (shadowDraw.instanceData.meshletCount == 0) {
                            passCtx.encoder.DrawInstanced<Shaders::Modules::BasicVSShadow>(
                                {.pipeline      = ctx.shadows.CascadePipeline(),
                                 .layout        = ctx.shadows.CascadeLayout(),
                                 .heap          = true,
                                 .vertexCount   = indirectCmdsBase[passWriteOffsets[0] + d].vertexCount,
                                 .instanceCount = 1,
                                 .firstVertex   = 0,
                                 .firstInstance = instanceIdx},
                                RenderContext::Impl::ObjectConstants {.instanceId = instanceIdx, .isShadowPass = 1},
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                            );
                            continue;
                        }

                        passCtx.encoder.DrawMeshTasks<Shaders::Modules::BasicTask>(
                            {.pipeline    = ctx.shadows.CascadeMeshPipeline(),
                             .layout      = ctx.shadows.CascadeLayout(),
                             .heap        = true,
                             .groupCountX = TaskGroupCount(shadowDraw.instanceData.meshletCount),
                             .groupCountY = 1,
                             .groupCountZ = 1},
                            RenderContext::Impl::ObjectConstants {.instanceId = instanceIdx, .isShadowPass = 1}
                        );
                    }
                } else if (csmDrawCount > 0) {
                    passCtx.encoder.DrawIndirect<Shaders::Modules::BasicVSShadow>(
                        {.pipeline       = ctx.shadows.CascadePipeline(),
                         .layout         = ctx.shadows.CascadeLayout(),
                         .heap           = true,
                         .argumentBuffer = ctx.shadows.IndirectCommands(frameIndex).Handle(),
                         .offset         = Vk::DrawIndirectState::OffsetForIndex(passWriteOffsets[0]),
                         .drawCount      = csmDrawCount},
                        RenderContext::Impl::ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 1},
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
                }

                if (hasMeshParticles) {
                    Draw3DParticleShadows(passCtx);
                }
            });
    }

    if (ctx.shadows.PunctualPipeline() != VK_NULL_HANDLE && !ctx.targets.PunctualViews().empty()) {
        auto ExecutePunctualPass = [&](const Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>& subViewImage, auto&& recordFn) {
            Vk::DynamicPass(subViewImage.extent)
                .ViewMask(ShadowRenderer::kCubemapFaceMask)
                .AddDepth(subViewImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ShadowRenderer::kShadowClearDepth)
                .Execute(cmd, std::forward<decltype(recordFn)>(recordFn));
        };

        for (uint32_t l_idx = 0; l_idx < ctx.mappedLights.size(); ++l_idx) {
            const auto& light = ctx.mappedLights[l_idx];
            if (light.shadowLayer < 0) {
                continue;
            }

            const uint32_t slotIdx   = kFirstPunctualSlot + light.shadowLayer;
            const uint32_t drawCount = passDrawCounts[slotIdx];

            if (drawCount == 0 && ctx.queues.MeshParticleEmitters().empty()) {
                continue;
            }

            const Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> subViewImage = {
                .handle = ctx.graphResources.shadowAtlas.image.Handle(),
                .view   = ctx.targets.PunctualViews()[light.shadowLayer].Get(),
                .extent = {.width = 1024, .height = 1024, .depth = 1},
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                .format = VK_FORMAT_D32_SFLOAT,
                .info   = &ctx.targets.PunctualViews()[light.shadowLayer].Info()
            };

            ExecutePunctualPass(subViewImage, [&]() {
                if (drawCount > 0) {
                    const PunctualPush pc = {l_idx};
                    passCtx.encoder.DrawIndirect<Shaders::Modules::PunctualShadowsVS>(
                        {
                            .pipeline       = ctx.shadows.PunctualPipeline(),
                            .layout         = ctx.shadows.PunctualLayout(),
                            .heap           = true,
                            .argumentBuffer = ctx.shadows.IndirectCommands(frameIndex).Handle(),
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

}
