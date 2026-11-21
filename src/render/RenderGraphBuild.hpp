// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "render/GBufferSurface.hpp"
#include "render/PassPayloads.hpp"
#include "render/RenderInternal.hpp"
#include "render/Renderer.hpp"
#include "vulkan/graph/RenderGraph.hpp"
#include "render/passes/ComputePass.hpp"
#include "render/passes/FullscreenPass.hpp"
#include "render/passes/aa/SMAAPasses.hpp"
#include "render/passes/culling/HiZGeneratePass.hpp"
#include "render/passes/forward/ForwardPass.hpp"
#include "render/passes/forward/OpaqueSceneCopyPass.hpp"
#include "render/passes/forward/TranslucentPrePass.hpp"
#include "render/passes/gbuffer/BackgroundGBufferPass.hpp"
#include "render/passes/gbuffer/DecalPass.hpp"
#include "render/passes/gbuffer/GBufferBasePass.hpp"
#include "render/passes/gbuffer/GBufferCullingPass.hpp"
#include "render/passes/gbuffer/GBufferResolvePass.hpp"
#include "render/passes/gbuffer/ViewmodelPass.hpp"
#include "render/passes/lighting/ClusteredLightingPass.hpp"
#include "render/passes/lighting/EmissivePass.hpp"
#include "render/passes/lighting/LightingMipPass.hpp"
#include "render/passes/lighting/RayTracingPasses.hpp"
#include "render/passes/lighting/ReflectionPass.hpp"
#include "render/passes/lighting/RtgiCompositePass.hpp"
#include "render/passes/lighting/ShadowPass.hpp"
#include "render/passes/lighting/SunShadowMaskPass.hpp"
#include "render/passes/postprocess/BloomPass.hpp"
#include "render/passes/postprocess/TonemapBlitPass.hpp"
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
        .camPos      = {view.worldPosition.x, view.worldPosition.y, view.worldPosition.z, view.time},
        .giMode      = settings.post.mode,
        .aoRadius    = settings.post.aoRadius,
        .aoBias      = settings.post.aoBias,
        .aoPower     = settings.post.aoPower,
        .giIntensity = settings.post.giIntensity,
        .giSamples   = settings.post.giSamples,
        .enableSSR   = settings.post.enableSSR,
        // `enableRTR` is this frame's answer to "may a shader trace against the
        // BVH at all?". It used to answer a narrower question -- "did the user
        // enable ray-traced *reflections*?" -- and the narrowing broke both of its
        // consumers:
        //
        //   * lighting.slang gated the sun contact ray on it, so a frame with
        //     enableShadows set and enableReflections clear traced no sun contact
        //     shadows: the shadow switch was dead, and the stability test that
        //     toggles enableReflections to observe the contact term was measuring
        //     a coupling between two settings that have nothing to do with each
        //     other;
        //   * it tested `frames.tlas[...]` (the acceleration structure) where the
        //     presence test belongs on `frames.tlasAddresses[...]` (the device
        //     address a shader actually traces through), so a built TLAS with no
        //     address still reported rays as live.
        //
        // A sun contact ray is a sun shadow, so it is live when *either switch is
        // on, and this is the one place that decides it. The TLAS stays the hard
        // requirement -- no acceleration structure, no rays, whatever the settings
        // say -- and `tlasAddresses` rather than `tlas` is what proves it, because
        // the address is what a shader traces through.
        .enableRTR   = (impl.frames.tlasAddresses[impl.presenter.frameIndex] &&
                        (settings.rayTracing.enableReflections || settings.rayTracing.enableShadows)) ?
                           1 :
                           0,
    };
}

// The AA filters' argument builders. Named rather than written as lambdas at
// the graph pack because a template argument cannot name a closure type; each
// returns the push payload and slot writes FullscreenPassRecorder records,
// exactly what the former FXAAPass wrapper built inline.
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

template <AAMode Mode>
[[nodiscard]] auto BuildFrameGraph(RenderContext::Impl& self, RenderFrame& frame, const ScenePushConstants& pc, SwapchainImageFn getSwapchain) {
    using OpaqueReflectionPass      = Passes::ReflectionPass<Passes::OpaqueSurface, Res_HdrSceneColor, "Reflection">;
    using TranslucentReflectionPass = Passes::ReflectionPass<Passes::TranslucentSurface, Res_TransLighting, "TransReflection">;

    auto core = Vk::MakePassPack(
        Passes::ShadowPass {.particles = self.meshParticles, .frame = frame}, Passes::GBufferCullingPass {.cull = self.culling, .frame = frame},
        Passes::GBufferBasePass {.cull = self.culling, .frame = frame},
        Passes::BackgroundGBufferPass {.frame = frame}, Passes::HiZGeneratePass {.cull = self.culling, .frame = frame},
        Passes::GBufferResolveCullingPass {.cull = self.culling, .frame = frame},
        Passes::GBufferResolvePass {.cull = self.culling, .csg = self.csg, .particles = self.meshParticles, .frame = frame},
        Passes::DecalPass {.decals = self.decals, .frame = frame}, Passes::ViewmodelPass {.frame = frame},
        Passes::TranslucentPrePass {.frame = frame},
        Passes::AoDepthGeneratePass {.cull = self.culling, .frame = frame},
        Passes::ComputePass<Passes::GtaoDesc> {.frame = frame, .pc = pc},
        // Order matters: the mask must exist before ClusteredLightingPass reads it.
        Passes::SunShadowMaskPass {.frame = frame}, Passes::ClusteredLightingPass {.post = self.post, .frame = frame, .pc = pc},
        Passes::RcgiCascadePass {.frame = frame}, Passes::RtgiCompositePass {.frame = frame}, Passes::EmissivePass {.forward = self.forward, .frame = frame},
        Passes::LightingMipPass {.frame = frame}, OpaqueReflectionPass {.frame = frame, .pc = pc}, TranslucentReflectionPass {.frame = frame, .pc = pc},
        Passes::OpaqueSceneCopyPass {.frame = frame}, Passes::ForwardPass {.forward = self.forward, .frame = frame}, Passes::BloomPass {.frame = frame}
    );

    auto aa = [&] {
        if constexpr (Mode == AAMode::FXAA) {
            return Vk::MakePassPack(
                Passes::FullscreenPass<
                    "FXAA", Shaders::Fxaa, Shaders::Modules::FxaaPS, Res_AccumCurrent, FXAAPushConstants, decltype(kFxaaPrepare), Res_HdrSceneColor> {
                    .frame = frame, .pipeline = self.antiAliasing.fxaa, .prepare = kFxaaPrepare
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
