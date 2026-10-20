// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "PassPayloads.hpp"
#include "TargetManager.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/ComputePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// The ray-tracing and screen-space lighting dispatches, one descriptor each:
// the same skeleton Passes::ComputePass records, with only the gate, the slot
// writes and the push payload differing between them. Bodies are the former
// per-pass operator() definitions, verbatim.

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

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants>& {
        return frame.postProcess.Gtao();
    }

    [[nodiscard]] static auto GetBindings(RenderFrameContext& frame) noexcept -> Vk::HeapPassBindings& {
        return frame.postProcess.GtaoHeapBindings();
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

struct RtShadowDesc {
    static constexpr auto name = Vk::ResourceName("RtShadow");
    using Usages               = Vk::TypeList<
        Vk::FragmentAndComputeRead<Res_Depth>,
        Vk::FragmentAndComputeRead<Res_NormRough>,
        Vk::ComputeWrite<Res_RtShadowMask>>;
    using ShaderDecl    = Shaders::RtShadow;
    using ShaderModule  = Shaders::Modules::RtShadowCS;
    using PushConstants = RTShadowPushConstants;

    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        const uint32_t fIdx   = frame.presenter.frameIndex;
        const bool     wantRT = (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) || frame.settings.rayTracing.enableShadows;
        return frame.ctx.RayTracingSupported() && wantRT && frame.settings.environment.fullBright == 0 && frame.frames.tlasAddresses[fIdx] != nullptr;
    }

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::TypedComputePass<Shaders::Modules::RtShadowCS, RTShadowPushConstants>& {
        return frame.postProcess.RtShadow();
    }

    [[nodiscard]] static auto GetBindings(RenderFrameContext& frame) noexcept -> Vk::HeapPassBindings& {
        return frame.postProcess.RtShadowHeapBindings();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
        const auto              push  = RTShadowPushConstants {
             .samples     = 1u,
             .normalBias  = 0.04f,
             .maxDistance = 300.0f,
        };

        return MakeComputePassArgs(
            push, VkExtent3D {frame.graphResources.rtShadowMask.extent.width, frame.graphResources.rtShadowMask.extent.height, 1},
            Vk::Slot<"texDepth">(Vk::Assume<Vk::FragmentAndComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
            Vk::Slot<"texNormalRoughness">(
                Vk::Assume<Vk::FragmentAndComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)
            ),
            Vk::Slot<"outShadow">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtShadowMask)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"tlas">(tlas),
            Vk::Slot<"blueNoiseTex">(frame.textureManager.View(frame.blueNoiseTexIdx))
        );
    }
};

struct RtrHalfTraceDesc {
    static constexpr auto name = Vk::ResourceName("RtrHalfTrace");
    using Usages               = Vk::TypeList<
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>,
        Vk::ComputeRead<Res_Lighting>,
        Vk::ComputeRead<Res_DdgiIrradiance>,
        Vk::ComputeRead<Res_DdgiDistance>,
        Vk::ComputeWrite<Res_RtrHalf>>;
    using ShaderDecl    = Shaders::RtrHalf;
    using ShaderModule  = Shaders::Modules::RtrHalfCS;
    using PushConstants = RtrHalfPushConstants;

    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        return frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR;
    }

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::TypedComputePass<Shaders::Modules::RtrHalfCS, RtrHalfPushConstants>& {
        return frame.postProcess.RtrHalf();
    }

    [[nodiscard]] static auto GetBindings(RenderFrameContext& frame) noexcept -> Vk::HeapPassBindings& {
        return frame.postProcess.RtrHalfHeapBindings();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
        const auto              push  = RtrHalfPushConstants {
             .halfRes = {frame.graphResources.rtrHalf.extent.width, frame.graphResources.rtrHalf.extent.height},
        };

        return MakeComputePassArgs(
            push, VkExtent3D {frame.graphResources.rtrHalf.extent.width, frame.graphResources.rtrHalf.extent.height, 1},
            Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
            Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
            Vk::Slot<"texLighting">(Vk::Assume<Vk::ComputeRead<Res_Lighting>>(frame.graphResources.lightingTarget)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
            Vk::Slot<"blueNoiseTex">(frame.textureManager.View(frame.blueNoiseTexIdx)),
            Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtrHalf)), Vk::Slot<"tlas">(tlas),
            Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
            Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance))
        );
    }
};

struct RtgiHalfTraceDesc {
    static constexpr auto name = Vk::ResourceName("RtgiHalfTrace");
    using Usages               = Vk::TypeList<
        Vk::ComputeRead<Res_Depth>,
        Vk::ComputeRead<Res_NormRough>,
        Vk::ComputeRead<Res_Lighting>,
        Vk::ComputeRead<Res_DdgiIrradiance>,
        Vk::ComputeRead<Res_DdgiDistance>,
        Vk::ComputeWrite<Res_RtgiHalf>>;
    using ShaderDecl    = Shaders::RtgiHalf;
    using ShaderModule  = Shaders::Modules::RtgiHalfCS;
    using PushConstants = RtgiHalfPushConstants;

    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;
        return frame.ctx.RayTracingSupported() && frame.settings.rayTracing.enableDiffuseGI && frame.settings.environment.fullBright == 0 &&
               frame.frames.tlasAddresses[fIdx] != nullptr;
    }

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::TypedComputePass<Shaders::Modules::RtgiHalfCS, RtgiHalfPushConstants>& {
        return frame.postProcess.RtgiHalf();
    }

    [[nodiscard]] static auto GetBindings(RenderFrameContext& frame) noexcept -> Vk::HeapPassBindings& {
        return frame.postProcess.RtgiHalfHeapBindings();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
        const auto              push  = RtgiHalfPushConstants {
             .halfRes = {frame.graphResources.rtgiHalf.extent.width, frame.graphResources.rtgiHalf.extent.height},
        };

        return MakeComputePassArgs(
            push, VkExtent3D {frame.graphResources.rtgiHalf.extent.width, frame.graphResources.rtgiHalf.extent.height, 1},
            Vk::Slot<"texDepth">(Vk::Assume<Vk::ComputeRead<Res_Depth>>(frame.ActiveDepthTarget())),
            Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ComputeRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]), Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
            Vk::Slot<"blueNoiseTex">(frame.textureManager.View(frame.blueNoiseTexIdx)),
            Vk::Slot<"outImage">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.rtgiHalf)), Vk::Slot<"tlas">(tlas),
            Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
            Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance))
        );
    }
};

struct DdgiTraceDesc {
    static constexpr auto name = Vk::ResourceName("DdgiRayTrace");
    using Usages               = Vk::TypeList<
        Vk::ComputeRead<Res_DdgiIrradiance>,
        Vk::ComputeRead<Res_DdgiDistance>,
        Vk::ComputeWrite<Res_DdgiRays>>;
    using ShaderDecl    = Shaders::DdgiTrace;
    using ShaderModule  = Shaders::Modules::DdgiTraceCS;
    using PushConstants = DdgiTracePushConstants;

    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;
        const bool     wantRT =
            frame.settings.rayTracing.enableDiffuseGI || (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) ||
            frame.settings.rayTracing.enableShadows;
        return frame.ctx.RayTracingSupported() && wantRT && frame.settings.environment.fullBright == 0 &&
               frame.frames.tlasAddresses[fIdx] != nullptr;
    }

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::TypedComputePass<Shaders::Modules::DdgiTraceCS, DdgiTracePushConstants>& {
        return frame.postProcess.DdgiTrace();
    }

    [[nodiscard]] static auto GetBindings(RenderFrameContext& frame) noexcept -> Vk::HeapPassBindings& {
        return frame.postProcess.DdgiTraceHeapBindings();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const Vk::AsAddressWrite tlas {.address = frame.frames.tlasAddresses[fIdx].Get()};
        const auto              totalProbes  = frame.graphResources.ddgiRays.extent.width;
        const auto              raysPerProbe = frame.graphResources.ddgiRays.extent.height;

        const DdgiTracePushConstants push {
            .totalProbes    = totalProbes,
            .raysPerProbe   = raysPerProbe,
            .maxRayDistance = 24.0f,
            ._pad           = 0u,
        };

        return MakeComputePassArgs(
            push, VkExtent3D {totalProbes, raysPerProbe, 1}, Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx]),
            Vk::Slot<"g_instances">(frame.frames.instanceDataBuffers[fIdx]),
            Vk::Slot<"texDdgiIrradiance">(Vk::Assume<Vk::ComputeRead<Res_DdgiIrradiance>>(frame.graphResources.ddgiIrradiance)),
            Vk::Slot<"texDdgiDistance">(Vk::Assume<Vk::ComputeRead<Res_DdgiDistance>>(frame.graphResources.ddgiDistance)),
            Vk::Slot<"outRays">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ddgiRays)), Vk::Slot<"tlas">(tlas)
        );
    }
};

struct DdgiUpdateDesc {
    static constexpr auto name = Vk::ResourceName("DdgiUpdateAtlas");
    using Usages               = Vk::TypeList<
        Vk::ComputeRead<Res_DdgiRays>,
        Vk::ComputeWrite<Res_DdgiIrradiance>,
        Vk::ComputeWrite<Res_DdgiDistance>>;
    using ShaderDecl    = Shaders::DdgiUpdateIrradiance;
    using ShaderModule  = Shaders::Modules::DdgiUpdateIrradianceCS;
    using PushConstants = DdgiUpdatePushConstants;

    [[nodiscard]] static bool ShouldRun(RenderFrameContext& frame) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;
        const bool     wantRT =
            frame.settings.rayTracing.enableDiffuseGI || (frame.settings.rayTracing.enableReflections && frame.settings.post.enableRTR != 0) ||
            frame.settings.rayTracing.enableShadows;
        return frame.ctx.RayTracingSupported() && wantRT && frame.settings.environment.fullBright == 0 &&
               frame.frames.tlasAddresses[fIdx] != nullptr;
    }

    [[nodiscard]] static auto GetPass(RenderFrameContext& frame) noexcept -> Vk::TypedComputePass<Shaders::Modules::DdgiUpdateIrradianceCS, DdgiUpdatePushConstants>& {
        return frame.postProcess.DdgiUpdate();
    }

    [[nodiscard]] static auto GetBindings(RenderFrameContext& frame) noexcept -> Vk::HeapPassBindings& {
        return frame.postProcess.DdgiUpdateHeapBindings();
    }

    [[nodiscard]] static auto Prepare(RenderFrameContext& frame, const Slang::ScenePassPushConstants&) noexcept {
        const uint32_t fIdx = frame.presenter.frameIndex;

        const auto totalProbes  = frame.graphResources.ddgiRays.extent.width;
        const auto raysPerProbe = frame.graphResources.ddgiRays.extent.height;

        const DdgiUpdatePushConstants push {
            .totalProbes    = totalProbes,
            .raysPerProbe   = raysPerProbe,
            .hysteresis     = frame.currentUniforms.ddgiParams.x,
            .depthSharpness = 16.0f,
        };

        return MakeComputePassArgs(
            push, VkExtent3D {totalProbes * 16u, 16u, 1u}, Vk::Slot<"inRays">(Vk::Assume<Vk::ComputeRead<Res_DdgiRays>>(frame.graphResources.ddgiRays)),
            Vk::Slot<"outIrradiance">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ddgiIrradiance)),
            Vk::Slot<"outDistance">(Vk::AssumeLayout<VK_IMAGE_LAYOUT_GENERAL>(frame.graphResources.ddgiDistance)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[fIdx])
        );
    }
};

} // namespace ZHLN::Passes
