// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "GBufferSurface.hpp"
#include "PassPayloads.hpp"
#include "RenderInternal.hpp"
#include "Renderer.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/ComputePass.hpp"
#include "passes/FullscreenPass.hpp"
#include "passes/aa/SMAAPasses.hpp"
#include "passes/culling/HiZGeneratePass.hpp"
#include "passes/forward/ForwardPass.hpp"
#include "passes/forward/OpaqueSceneCopyPass.hpp"
#include "passes/forward/TranslucentPrePass.hpp"
#include "passes/gbuffer/BackgroundGBufferPass.hpp"
#include "passes/gbuffer/DecalPass.hpp"
#include "passes/gbuffer/GBufferBasePass.hpp"
#include "passes/gbuffer/GBufferResolvePass.hpp"
#include "passes/gbuffer/ViewmodelPass.hpp"
#include "passes/lighting/ClusteredLightingPass.hpp"
#include "passes/lighting/EmissivePass.hpp"
#include "passes/lighting/LightingMipPass.hpp"
#include "passes/lighting/RayTracingPasses.hpp"
#include "passes/lighting/ReflectionPass.hpp"
#include "passes/lighting/RtgiCompositePass.hpp"
#include "passes/lighting/ShadowPass.hpp"
#include "passes/postprocess/BloomPass.hpp"
#include "passes/postprocess/TonemapBlitPass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/View.hpp>
#include <utility>

namespace ZHLN::RenderGraphDetail {

using SwapchainImage     = Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>;
using SwapchainImageFn   = FunctionRef<SwapchainImage() const>;
using ScenePushConstants = Slang::ScenePassPushConstants;

[[nodiscard]] inline auto
    AssembleScenePushConstants(const RenderContext::Impl& impl, const SceneView& view, const GraphicsSettings& settings) noexcept -> ScenePushConstants {
    return ScenePushConstants {
        .invViewProj = view.invViewProjMatrix,
        .viewProj    = view.viewProjMatrix,
        .camPos      = {view.worldPosition.GetX(), view.worldPosition.GetY(), view.worldPosition.GetZ(), view.time},
        .giMode      = settings.post.mode,
        .aoRadius    = settings.post.aoRadius,
        .aoBias      = settings.post.aoBias,
        .aoPower     = settings.post.aoPower,
        .giIntensity = settings.post.giIntensity,
        .giSamples   = settings.post.giSamples,
        .enableSSR   = settings.post.enableSSR,
        .enableRTR   = (impl.frames.tlas[impl.presenter.frameIndex] && (settings.rayTracing.enableReflections || settings.rayTracing.enableShadows)) ?
                           settings.post.enableRTR :
                           0,
    };
}

// The AA filters' argument builders. Named rather than written as lambdas at
// the graph pack because a template argument cannot name a closure type; each
// returns the push payload and slot writes FullscreenPassRecorder records,
// exactly what the former FXAAPass/MLAAPass/TAAPass wrappers built inline.
inline constexpr auto kTaaPrepare = +[](RenderFrame& frame) noexcept {
    return Passes::MakeFullscreenPassArgs(
        TAAPushConstants {.feedback = frame.settings.antiAliasing.taaFeedback},
        Vk::Slot<"texCurrent">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(frame.graphResources.hdrSceneColor)),
        Vk::Slot<"texHistory">(Vk::Assume<Vk::ShaderRead<Res_AccumPrevious>>(frame.renderer.accumulationHistory.Previous())),
        Vk::Slot<"texVelocity">(Vk::Assume<Vk::ShaderRead<Res_Velocity>>(frame.graphResources.velocityBuffer)), Vk::Slot<"frame">(frame.frameUniformBuffers)
    );
};

inline constexpr auto kFxaaPrepare = +[](RenderFrame& frame) noexcept {
    const auto& inputColor = frame.graphResources.hdrSceneColor;
    const auto  rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
    const auto  rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

    return Passes::MakeFullscreenPassArgs(
        FXAAPushConstants {
            .rcpFrameX        = rcpW,
            .rcpFrameY        = rcpH,
            .subpix           = frame.settings.antiAliasing.fxaaSubpix,
            .edgeThreshold    = frame.settings.antiAliasing.fxaaEdgeThreshold,
            .edgeThresholdMin = frame.settings.antiAliasing.fxaaEdgeThresholdMin,
            ._pad             = 0.0f
        },
        Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
    );
};

inline constexpr auto kMlaaPrepare = +[](RenderFrame& frame) noexcept {
    const auto& inputColor = frame.graphResources.hdrSceneColor;
    const auto  rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
    const auto  rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

    return Passes::MakeFullscreenPassArgs(
        MLAAPushConstants {
            .rcpFrameX      = rcpW,
            .rcpFrameY      = rcpH,
            .threshold      = frame.settings.antiAliasing.mlaaThreshold,
            .maxSearchSteps = frame.settings.antiAliasing.mlaaMaxSearchSteps
        },
        Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
    );
};

template <AAMode Mode>
[[nodiscard]] auto BuildFrameGraph(RenderContext::Impl& self, RenderFrame& frame, const ScenePushConstants& pc, SwapchainImageFn getSwapchain) {
    using OpaqueReflectionPass      = Passes::ReflectionPass<Passes::OpaqueSurface, Res_HdrSceneColor, "Reflection">;
    using TranslucentReflectionPass = Passes::ReflectionPass<Passes::TranslucentSurface, Res_TransLighting, "TransReflection">;

    auto core = Vk::MakePassPack(
        Passes::ShadowPass {.particles = self.meshParticles, .frame = frame}, Passes::GBufferBasePass {.cull = self.culling, .frame = frame},
        Passes::BackgroundGBufferPass {.frame = frame}, Passes::HiZGeneratePass {.cull = self.culling, .frame = frame},
        Passes::GBufferResolvePass {.cull = self.culling, .csg = self.csg, .particles = self.meshParticles, .frame = frame},
        Passes::DecalPass {.decals = self.decals, .frame = frame}, Passes::ViewmodelPass {.frame = frame},
        Passes::SsrHiZGeneratePass {.cull = self.culling, .frame = frame}, Passes::TranslucentPrePass {.frame = frame},
        Passes::ComputePass<Passes::GtaoDesc> {.frame = frame, .pc = pc}, Passes::ClusteredLightingPass {.post = self.post, .frame = frame, .pc = pc},
        Passes::RcgiCascadePass {.frame = frame}, Passes::RtgiCompositePass {.frame = frame}, Passes::EmissivePass {.forward = self.forward, .frame = frame},
        Passes::LightingMipPass {.frame = frame}, OpaqueReflectionPass {.frame = frame, .pc = pc}, TranslucentReflectionPass {.frame = frame, .pc = pc},
        Passes::OpaqueSceneCopyPass {.frame = frame}, Passes::ForwardPass {.forward = self.forward, .frame = frame}, Passes::BloomPass {.frame = frame}
    );

    auto aa = [&] {
        if constexpr (Mode == AAMode::TAA) {
            return Vk::MakePassPack(Passes::FullscreenPass<
                                    "TAA", Shaders::Taa, Shaders::Modules::TaaPS, Res_AccumCurrent, TAAPushConstants, decltype(kTaaPrepare), Res_HdrSceneColor,
                                    Res_Velocity, Res_Depth, Res_AccumPrevious> {.frame = frame, .pipeline = self.antiAliasing.taa, .prepare = kTaaPrepare});
        } else if constexpr (Mode == AAMode::FXAA) {
            return Vk::MakePassPack(
                Passes::FullscreenPass<
                    "FXAA", Shaders::Fxaa, Shaders::Modules::FxaaPS, Res_AccumCurrent, FXAAPushConstants, decltype(kFxaaPrepare), Res_HdrSceneColor> {
                    .frame = frame, .pipeline = self.antiAliasing.fxaa, .prepare = kFxaaPrepare
                }
            );
        } else if constexpr (Mode == AAMode::MLAA) {
            return Vk::MakePassPack(
                Passes::FullscreenPass<
                    "MLAA", Shaders::Mlaa, Shaders::Modules::MlaaPS, Res_AccumCurrent, MLAAPushConstants, decltype(kMlaaPrepare), Res_HdrSceneColor> {
                    .frame = frame, .pipeline = self.antiAliasing.mlaa, .prepare = kMlaaPrepare
                }
            );
        } else if constexpr (Mode == AAMode::SMAA) {
            return Vk::MakePassPack(
                Passes::SmaaEdgePass {.aa = self.antiAliasing, .frame = frame}, Passes::SmaaWeightPass {.aa = self.antiAliasing, .frame = frame},
                Passes::SmaaBlendPass {.aa = self.antiAliasing, .frame = frame}
            );
        } else {
            return Vk::MakePassPack();
        }
    }();

    auto blit = Vk::MakePassPack(Passes::TonemapBlitPass<Mode> {.post = self.post, .frame = frame, .getSwapchainImage = getSwapchain});

    return (std::move(core) + std::move(aa) + std::move(blit)).BuildGraph();
}

template <AAMode Mode>
using SceneGraphFor = decltype(BuildFrameGraph<Mode>(
    std::declval<RenderContext::Impl&>(),
    std::declval<RenderFrame&>(),
    std::declval<const ScenePushConstants&>(),
    std::declval<SwapchainImageFn>()
));

} // namespace ZHLN::RenderGraphDetail
