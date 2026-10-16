// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ShadowPass.hpp"
#include "features/ShadowAtlas.hpp"
#include "features/ShadowRenderer.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Math3D.hpp>

namespace ZHLN::Passes {

namespace {

struct ShadowTilePush {
    JPH::Mat44 viewProj;
};
static_assert(GpuAbi::ScenePassPayload<ShadowTilePush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

constexpr uint32_t kCascadeSlot    = 0;
constexpr uint32_t kFirstAtlasSlot = 1;

[[nodiscard]] constexpr auto IndirectOffsetForSlot(uint32_t slot) noexcept -> uint32_t {
    return slot == kCascadeSlot ? 0 : kGpuCullingMaxInstances + (slot - kFirstAtlasSlot) * ShadowRenderer::kAtlasInstancesPerLight;
}

} // namespace

void ShadowPass::operator()(VkCommandBuffer cmd) const noexcept {
    PassContext    passCtx(cmd, frame, frame.InheritsHeaps());
    auto&          ctx        = passCtx.ctx;
    const uint32_t frameIndex = ctx.presenter.frameIndex;

    passCtx.EnsureHeapState();

    std::array<Frustum, TargetManager::kCascades> cascadeFrustums {};
    for (uint32_t c = 0; c < TargetManager::kCascades; ++c) {
        cascadeFrustums[c].Update(ctx.currentUniforms.lightSpaceMatrices[c]);
    }

    auto mapped = ctx.shadows.IndirectCommands(frameIndex).Map(ctx.allocator);
    if (!mapped) {
        return;
    }
    auto* indirectCmdsBase = mapped->As<VkDrawIndirectCommand>();

    const auto placements = ctx.shadowAtlas.Placements();

    bool anyMovedCaster = false;

    std::array<uint32_t, ShadowAtlas::kMaxLights + 1> passDrawCounts {};

    for (uint32_t i = 0; i < ctx.queues.Draws().size(); ++i) {
        const auto& drawCmd = ctx.queues.Draws()[i];

        if (!IsVisibleIn(drawCmd.flags, RenderPassType::Shadow) || IsForwardOnly(drawCmd.instanceData.flags)) {
            continue;
        }

        if (!anyMovedCaster) {
            const bool skinned = (drawCmd.instanceData.flags & (1u << 8)) != 0u;
            const bool morphed = drawCmd.activeMorphCount > 0;
            anyMovedCaster     = skinned || morphed || !(drawCmd.instanceData.prevWorld == drawCmd.instanceData.world);
        }

        const uint32_t  vertexCount = drawCmd.instanceData.iboAddress != 0 ? drawCmd.instanceData.indexCount : drawCmd.instanceData.vertexCount;
        const JPH::Vec3 meshPos     = drawCmd.instanceData.world.GetTranslation();
        const float     radius      = drawCmd.instanceData.cullRadius;

        bool inAnyCascade = false;
        for (uint32_t c = 0; c < TargetManager::kCascades; ++c) {
            if (cascadeFrustums[c].IsSphereVisible(meshPos, radius)) {
                inAnyCascade = true;
                break;
            }
        }
        if (inAnyCascade) {
            const uint32_t writeIdx    = IndirectOffsetForSlot(kCascadeSlot) + passDrawCounts[kCascadeSlot];
            indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
            passDrawCounts[kCascadeSlot]++;
        }

        for (uint32_t p = 0; p < placements.size(); ++p) {
            const auto& placement = placements[p];
            if (placement.tileBase < 0 || placement.lightIndex < 0) {
                continue;
            }
            const uint32_t slot = kFirstAtlasSlot + p;
            if (slot >= passDrawCounts.size() || passDrawCounts[slot] >= ShadowRenderer::kAtlasInstancesPerLight) {
                continue;
            }

            const auto& light = ctx.mappedLights[static_cast<size_t>(placement.lightIndex)];

            const JPH::Vec3 lightPos {light.position};
            const float     distToLightSq = (meshPos - lightPos).LengthSq();
            const float     maxRange      = light.range + radius;
            if (distToLightSq > maxRange * maxRange) {
                continue;
            }

            const uint32_t writeIdx    = IndirectOffsetForSlot(slot) + passDrawCounts[slot];
            indirectCmdsBase[writeIdx] = {.vertexCount = vertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = i};
            passDrawCounts[slot]++;
        }
    }

    {
        const bool hasMeshParticles = !ctx.queues.MeshParticleEmitters().empty();

        const bool multiviewMesh     = ctx.ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>([](const VkPhysicalDeviceMeshShaderFeaturesEXT& f) -> bool {
            return f.multiviewMeshShader == VK_TRUE;
        });
        const bool useMeshShadowPath = ctx.MeshShadingActive() && multiviewMesh && ctx.shadows.CascadeMeshPipeline() != VK_NULL_HANDLE;

        const uint32_t csmDrawCount = passDrawCounts[kCascadeSlot];

        const auto shadowMapArrayImage = ctx.graphResources.shadowMap.AsSlice().Assume<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>();

        Vk::DynamicPass(shadowMapArrayImage.Extent())
            .ViewMask(ShadowRenderer::kCascadeViewMask)
            .AddDepth(shadowMapArrayImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ShadowRenderer::kShadowClearDepth)
            .Execute(cmd, [&]() {
                const bool useMeshShadows = useMeshShadowPath && csmDrawCount > 0;

                if (useMeshShadows) {
                    for (uint32_t d = 0; d < csmDrawCount; ++d) {
                        const uint32_t instanceIdx = indirectCmdsBase[IndirectOffsetForSlot(kCascadeSlot) + d].firstInstance;
                        if (instanceIdx >= ctx.queues.Draws().size()) {
                            continue;
                        }
                        const auto& shadowDraw = ctx.queues.Draws()[instanceIdx];
                        if (shadowDraw.instanceData.meshletCount == 0) {
                            passCtx.encoder.DrawInstanced<Shaders::Modules::BasicVSShadow>(
                                {.pipeline      = ctx.shadows.CascadePipeline(),
                                 .vertexCount   = indirectCmdsBase[IndirectOffsetForSlot(kCascadeSlot) + d].vertexCount,
                                 .instanceCount = 1,
                                 .firstVertex   = 0,
                                 .firstInstance = instanceIdx},
                                ObjectConstants {.instanceId = instanceIdx, .isShadowPass = 1}
                            );
                            continue;
                        }

                        passCtx.encoder.DrawMeshTasks<Shaders::Modules::BasicTask>(
                            {.pipeline    = ctx.shadows.CascadeMeshPipeline(),
                             .groupCountX = TaskGroupCount(shadowDraw.instanceData.meshletCount),
                             .groupCountY = 1,
                             .groupCountZ = 1},
                            ObjectConstants {.instanceId = instanceIdx, .isShadowPass = 1}
                        );
                    }
                } else if (csmDrawCount > 0) {
                    passCtx.encoder.DrawIndirect<Shaders::Modules::BasicVSShadow>(
                        {.pipeline       = ctx.shadows.CascadePipeline(),
                         .argumentBuffer = ctx.shadows.IndirectCommands(frameIndex).Handle(),
                         .offset         = Vk::DrawIndirectState::OffsetForIndex(IndirectOffsetForSlot(kCascadeSlot)),
                         .drawCount      = csmDrawCount},
                        ObjectConstants {.instanceId = kGpuCullingSentinel, .isShadowPass = 1}
                    );
                }

                if (hasMeshParticles) {
                    Draw3DParticleShadows(passCtx, particles);
                }
            });
    }

    if (ctx.shadows.AtlasPipeline() != VK_NULL_HANDLE && !placements.empty()) {
        const auto atlasImage =
            Vk::ImageSlice {
                ctx.graphResources.shadowAtlas.image.Handle(), ctx.targets.AtlasView(),
                VkExtent2D {TargetManager::kShadowAtlasSize, TargetManager::kShadowAtlasSize}, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT
            }
                .Assume<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>();

        const auto tiles = ctx.shadowAtlas.Tiles();

        Vk::DynamicPass(atlasImage.Extent())
            .AddDepth(atlasImage, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, ShadowRenderer::kShadowClearDepth)
            .Execute(cmd, [&]() {
                for (uint32_t p = 0; p < placements.size(); ++p) {
                    const auto& placement = placements[p];
                    if (placement.tileBase < 0) {
                        continue;
                    }
                    const auto drawCount = passDrawCounts[kFirstAtlasSlot + p];

                    if (placement.cached && !anyMovedCaster) {
                        continue;
                    }

                    const auto tileBase = static_cast<uint32_t>(placement.tileBase);
                    const auto faces    = std::min<uint32_t>(placement.faceCount, ShadowAtlas::kPointFaces);

                    for (uint32_t face = 0; face < faces; ++face) {
                        const uint32_t tileIndex = tileBase + face;
                        if (tileIndex >= tiles.size()) {
                            break;
                        }
                        const ShadowTile            tile = tiles[tileIndex];
                        const ShadowAtlas::TileRect rect = ShadowAtlas::TileRectOf(tile);

                        Vk::ClearAttachment(
                            cmd, VK_IMAGE_ASPECT_DEPTH_BIT, VkExtent2D {rect.size, rect.size}, VkClearValue {.depthStencil = {.depth = 1.0f, .stencil = 0}}
                        );

                        if (drawCount == 0) {
                            continue;
                        }

                        passCtx.encoder.SetViewport(
                            VkViewport {
                                static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.size), static_cast<float>(rect.size), 0.0f, 1.0f
                            }
                        );
                        passCtx.encoder.SetScissor(
                            VkRect2D {.offset = {static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y)}, .extent = {rect.size, rect.size}}
                        );

                        const ShadowTilePush push {tile.viewProj};
                        passCtx.encoder.DrawIndirect<Shaders::Modules::ShadowAtlasVS>(
                            {.pipeline       = ctx.shadows.AtlasPipeline(),
                             .argumentBuffer = ctx.shadows.IndirectCommands(frameIndex).Handle(),
                             .offset         = Vk::DrawIndirectState::OffsetForIndex(IndirectOffsetForSlot(kFirstAtlasSlot + p)),
                             .drawCount      = drawCount},
                            push
                        );
                    }
                }
            });
    }
}

} // namespace ZHLN::Passes
