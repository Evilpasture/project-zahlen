// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "PassPayloads.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>
#include <array>
#include <cstdint>

namespace ZHLN::Passes {

// The ray-tracing and screen-space lighting dispatches, one descriptor each:
// the same skeleton Passes::ComputePass records, with only the gate, the slot
// writes and the push payload differing between them. Bodies are the former
// per-pass operator() definitions, verbatim.
//
// Each descriptor names its resource once. GetPass returns the LightingResources
// bundle -- pipeline and heap bindings together -- so the executor reads both
// halves off a single answer instead of asking for the pass and then asking
// again for the bindings that belong to it.

struct GtaoDesc {
    static constexpr auto name = Vk::ResourceName("GTAO");
    using Usages               = Vk::TypeList<Vk::FragmentAndComputeRead<Res_Depth>, Vk::FragmentAndComputeRead<Res_NormRough>, Vk::ComputeWrite<Res_Ao>>;
    using ShaderDecl           = Shaders::Gtao;
    using ShaderModule         = Shaders::Modules::GtaoCS;
    using PushConstants        = GtaoPushConstants;

    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        const int giMode = frame.settings.post.mode;
        return giMode == 3 || giMode == 4;
    }

    [[nodiscard]] static auto
        GetPass(RenderFrameContext& frame) noexcept -> Vk::BoundComputePass<Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants>>& {
        return frame.lighting.gtao;
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants& pc) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const auto& fullExt = frame.ActiveDepthTarget().extent;
        const auto  push    = GtaoPushConstants {
               .halfRes     = {frame.graphResources.ao.extent.width, frame.graphResources.ao.extent.height},
               .rcpFullRes  = {1.0f / static_cast<float>(fullExt.width), 1.0f / static_cast<float>(fullExt.height)},
               .time        = pc.camPos.w,
               .aoRadius    = pc.aoRadius,
               .aoBias      = pc.aoBias,
               .aoPower     = pc.aoPower,
               .giSamples   = static_cast<uint32_t>(pc.giSamples),
               .invViewProj = pc.invViewProj,
               .viewProj    = pc.viewProj,
        };

        return MakeComputePassArgs(
            push, VkExtent3D {frame.graphResources.ao.extent.width, frame.graphResources.ao.extent.height, 1},
            Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
            Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"outAo">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ao))
        );
    }
};

// Deterministic 4-cascade Radiance Cascades Global Illumination (RCGI):
// 1. Hierarchical surface-pinned probe setup (C0..C3) into Res_RcgiProbes.
// 2. Top-down Direction-First wave-coherent RayQuery trace + parallax-corrected
//    bilateral merge (C3 -> B, C2 -> A, C1 -> B, C0 -> A), converging in a
//    single frame with zero temporal history buffers or A-Trous denoising.
struct RcgiCascadePass:
    Vk::RenderPass<
        "RcgiCascade",
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>,
        Vk::ComputeRead<Res_ShadowAtlas>,
        Vk::ComputeWrite<Res_RcgiProbes>,
        Vk::ComputeWrite<Res_RcgiCascadeA>,
        Vk::ComputeWrite<Res_RcgiCascadeB>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;
        const bool     active =
            frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI && frame.settings.post.giIntensity > 0.0f &&
            frame.settings.environment.fullBright == 0 && frame.frames.tlasAddresses[fIdx] != nullptr;
        if (!active) {
            return;
        }

        const VkExtent2D fullExt    = frame.graphResources.lightingGI.extent;
        const VkExtent2D alignedExt = frame.graphResources.rcgiCascadeA.extent;
        if (fullExt.width == 0u || fullExt.height == 0u || alignedExt.width == 0u || alignedExt.height == 0u) {
            return;
        }

        const auto depth    = Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActiveDepthTarget());
        const auto normal   = Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer);
        const auto probes   = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rcgiProbes);
        const auto cascadeA = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rcgiCascadeA);
        const auto cascadeB = Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rcgiCascadeB);

        const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};

        Vk::ComputeChain chain(frame.heapManager, cmd);

        // Step 1: Surface-pinned probe pyramid C0 -> C1 -> C2 -> C3
        for (uint32_t c = 0u; c < 4u; ++c) {
            const uint32_t probeW = std::max(1u, alignedExt.width >> (c + 2u));
            const uint32_t probeH = std::max(1u, alignedExt.height >> (c + 2u));
            const auto     push   = RcgiProbeSetupPushConstants {
                .fullRes      = {fullExt.width, fullExt.height},
                .alignedRes   = {alignedExt.width, alignedExt.height},
                .cascadeIndex = c,
            };

            chain.Step<Shaders::RcgiProbeSetup>(
                frame.lighting.rcgiProbeSetup, VkExtent3D {probeW, probeH, 1u}, push,
                Vk::Slot<"texDepth">(depth),
                Vk::Slot<"texNormalRoughness">(normal),
                Vk::Slot<"inProbes">(probes),
                Vk::Slot<"outProbes">(probes),
                Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
            );
        }

        // Step 2: Top-down wave-coherent RayQuery trace & parallax-corrected bilateral merge
        struct CascadeInterval {
            float tMin;
            float tMax;
            float tUpperMax;
        };
        static constexpr std::array<CascadeInterval, 4> kIntervals {{
            {.tMin = 0.015f, .tMax = 0.65f, .tUpperMax = 2.60f},
            {.tMin = 0.550f, .tMax = 2.60f, .tUpperMax = 10.4f},
            {.tMin = 2.200f, .tMax = 10.4f, .tUpperMax = 40.0f},
            {.tMin = 8.800f, .tMax = 40.0f, .tUpperMax = 40.0f},
        }};

        for (int c = 3; c >= 0; --c) {
            const uint32_t cascIdx  = static_cast<uint32_t>(c);
            const auto&    interval = kIntervals[cascIdx];
            const auto&    inUpper  = ((cascIdx & 1u) != 0u) ? cascadeA : cascadeB;
            const auto&    outCasc  = ((cascIdx & 1u) != 0u) ? cascadeB : cascadeA;

            const auto push = RcgiCascadePushConstants {
                .fullRes        = {fullExt.width, fullExt.height},
                .alignedRes     = {alignedExt.width, alignedExt.height},
                .cascadeIndex   = cascIdx,
                .isTopCascade   = (cascIdx == 3u) ? 1u : 0u,
                .tMin           = interval.tMin,
                .tMax           = interval.tMax,
                .tUpperMax      = interval.tUpperMax,
                .normalBias     = 0.025f,
                .enablePunctual = 1u,
            };

            chain.Step<Shaders::RcgiCascadeTraceMerge>(
                frame.lighting.rcgiCascadeTraceMerge, VkExtent3D {alignedExt.width, alignedExt.height, 1u}, push,
                Vk::Slot<"inProbes">(probes),
                Vk::Slot<"inUpperCascade">(inUpper),
                Vk::Slot<"outCascade">(outCasc),
                Vk::Slot<"tlas">(tlas),
                Vk::Slot<"shadowAtlas">(frame.graphResources.shadowAtlas.image.View(VK_IMAGE_ASPECT_DEPTH_BIT)),
                Vk::Slot<"shadowTiles">(frame.frames.shadowTileBuffers[fIdx]),
                Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
                Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
                Vk::Slot<"lights">(frame.frames.lightStorageBuffers[fIdx])
            );
        }
    }
};

} // namespace ZHLN::Passes
