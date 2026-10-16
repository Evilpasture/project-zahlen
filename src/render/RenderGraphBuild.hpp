// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/aa/FXAAPass.hpp"
#include "passes/aa/MLAAPass.hpp"
#include "passes/aa/SMAAPasses.hpp"
#include "passes/aa/TAAPass.hpp"
#include "passes/culling/HiZGeneratePass.hpp"
#include "passes/forward/ForwardPass.hpp"
#include "passes/forward/OpaqueSceneCopyPass.hpp"
#include "passes/forward/TranslucentPrePass.hpp"
#include "passes/gbuffer/DecalPass.hpp"
#include "passes/gbuffer/GBufferBasePass.hpp"
#include "passes/gbuffer/GBufferResolvePass.hpp"
#include "passes/gbuffer/ViewmodelPass.hpp"
#include "passes/lighting/ClusteredLightingPass.hpp"
#include "passes/lighting/DdgiRayTracePass.hpp"
#include "passes/lighting/DdgiUpdateAtlasPass.hpp"
#include "passes/lighting/GtaoPass.hpp"
#include "passes/lighting/ReflectionPass.hpp"
#include "passes/lighting/RtShadowPass.hpp"
#include "passes/lighting/RtgiCompositePass.hpp"
#include "passes/lighting/RtgiDenoisePass.hpp"
#include "passes/lighting/RtgiHalfTracePass.hpp"
#include "passes/lighting/RtrDenoisePass.hpp"
#include "passes/lighting/RtrHalfTracePass.hpp"
#include "passes/lighting/ShadowDenoisePass.hpp"
#include "passes/lighting/ShadowPass.hpp"
#include "passes/postprocess/BloomPass.hpp"
#include "passes/postprocess/TonemapBlitPass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/View.hpp>
#include <utility>

namespace ZHLN::RenderGraphDetail {

using SwapchainImage     = Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>;
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
        ._pad        = {},
    };
}

template <AAMode Mode, typename GetSwapchainImageT>
[[nodiscard]] auto BuildFrameGraph(RenderContext::Impl& self, const ScenePushConstants& pc, GetSwapchainImageT&& getSwapchain) {
    using OpaqueReflectionPass      = Passes::ReflectionPass<Passes::OpaqueSurface, Res_HdrSceneColor, "Reflection">;
    using TranslucentReflectionPass = Passes::ReflectionPass<Passes::TranslucentSurface, Res_TransLighting, "TransReflection">;

    auto core = Vk::MakePassPack(
        Passes::ShadowPass {.particles = self.meshParticles, .frame = self}, Passes::GBufferBasePass {.cull = self.culling, .frame = self},
        Passes::HiZGeneratePass {.cull = self.culling, .frame = self},
        Passes::GBufferResolvePass {.cull = self.culling, .csg = self.csg, .particles = self.meshParticles, .frame = self},
        Passes::DecalPass {.decals = self.decals, .frame = self}, Passes::ViewmodelPass {.frame = self}, Passes::TranslucentPrePass {.frame = self},
        Passes::GtaoPass {.frame = self, .pc = pc}, Passes::DdgiRayTracePass {.frame = self}, Passes::DdgiUpdateAtlasPass {.frame = self},
        Passes::RtShadowPass {.impl = self}, Passes::ShadowDenoisePass {.impl = self},
        Passes::ClusteredLightingPass {.post = self.post, .frame = self, .pc = pc}, Passes::RtrHalfTracePass {.impl = self},
        Passes::RtgiHalfTracePass {.impl = self}, Passes::RtrDenoisePass {.impl = self}, Passes::RtgiDenoisePass {.impl = self},
        Passes::RtgiCompositePass {.impl = self}, OpaqueReflectionPass {.impl = self, .pc = pc}, TranslucentReflectionPass {.impl = self, .pc = pc},
        Passes::OpaqueSceneCopyPass {.impl = self}, Passes::ForwardPass {.forward = self.forward, .frame = self}, Passes::BloomPass {.impl = self}
    );

    auto aa = [&] {
        if constexpr (Mode == AAMode::TAA) {
            return Vk::MakePassPack(Passes::TAAPass {.aa = self.antiAliasing, .frame = self});
        } else if constexpr (Mode == AAMode::FXAA) {
            return Vk::MakePassPack(Passes::FXAAPass {.aa = self.antiAliasing, .frame = self});
        } else if constexpr (Mode == AAMode::MLAA) {
            return Vk::MakePassPack(Passes::MLAAPass {.aa = self.antiAliasing, .frame = self});
        } else if constexpr (Mode == AAMode::SMAA) {
            return Vk::MakePassPack(
                Passes::SmaaEdgePass {.aa = self.antiAliasing, .frame = self}, Passes::SmaaWeightPass {.aa = self.antiAliasing, .frame = self},
                Passes::SmaaBlendPass {.aa = self.antiAliasing, .frame = self}
            );
        } else {
            return Vk::MakePassPack();
        }
    }();

    auto blit = Vk::MakePassPack(
        Passes::TonemapBlitPass<Mode, std::decay_t<GetSwapchainImageT>> {
            .post = self.post, .frame = self, .getSwapchainImage = std::forward<GetSwapchainImageT>(getSwapchain)
        }
    );

    return (std::move(core) + std::move(aa) + std::move(blit)).BuildGraph();
}

template <AAMode Mode>
using SceneGraphFor =
    decltype(BuildFrameGraph<Mode>(std::declval<RenderContext::Impl&>(), std::declval<const ScenePushConstants&>(), std::declval<SwapchainImage (*)()>()));

} // namespace ZHLN::RenderGraphDetail
