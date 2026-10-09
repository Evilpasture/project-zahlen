// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The scene DAG declaration. Shared by RecordSceneFrame and GetRenderGraphDump
// so the dump can live in its own TU: GraphVisualizer instantiation is heavy,
// and keeping it out of RenderGraphBuilder.cpp means editing execute/record
// no longer rebuilds the visualizer in the same object file.

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
#include "passes/lighting/GtaoPass.hpp"
#include "passes/lighting/ReflectionPass.hpp"
#include "passes/lighting/RtrHalfTracePass.hpp"
#include "passes/lighting/ShadowPass.hpp"
#include "passes/postprocess/BloomPass.hpp"
#include "passes/postprocess/HdrDenoisePass.hpp"
#include "passes/postprocess/TonemapBlitPass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/View.hpp>
#include <utility>

namespace ZHLN::RenderGraphDetail {

using SwapchainImage     = Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>;
using ScenePushConstants = GeneratedGpu::ScenePassPushConstants;

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
        .enableRTR   = (impl.frames.tlas[impl.presenter.frameIndex] && settings.rayTracing.enableReflections) ? settings.post.enableRTR : 0,
        ._pad        = {},
    };
}

// The scene graph for one anti-aliasing configuration.
//
// The declaration order below is the order the passes run in, but the
// dependencies between them come from their declared usages, not from this
// list. The engine proves all of that at compile time and emits the barriers;
// nothing here encodes a dependency twice.
template <AAMode Mode, typename GetSwapchainImageT>
[[nodiscard]] auto BuildFrameGraph(RenderContext::Impl& self, const ScenePushConstants& pc, GetSwapchainImageT&& getSwapchain) {
    using OpaqueReflectionPass      = Passes::ReflectionPass<Passes::OpaqueSurface, Res_HdrSceneColor, "Reflection">;
    using TranslucentReflectionPass = Passes::ReflectionPass<Passes::TranslucentSurface, Res_TransLighting, "TransReflection">;

    // 1. Rasterization and deferred lighting DAG.
    //
    // The designators are load-bearing, not decoration. Every pass inherits
    // `Vk::RenderPass<...>`, which is an aggregate with no data members, so a
    // positional `{self}` would be read as an initializer for that base rather
    // than for the pass's own `impl`.
    auto core = Vk::MakePassPack(
        Passes::ShadowPass {.impl = self}, Passes::GBufferBasePass {.impl = self}, Passes::HiZGeneratePass {.impl = self},
        Passes::GBufferResolvePass {.impl = self}, Passes::DecalPass {.impl = self}, Passes::ViewmodelPass {.impl = self},
        Passes::TranslucentPrePass {.impl = self}, Passes::GtaoPass {.impl = self, .pc = pc}, Passes::ClusteredLightingPass {.impl = self, .pc = pc},
        Passes::RtrHalfTracePass {.impl = self}, OpaqueReflectionPass {.impl = self, .pc = pc}, TranslucentReflectionPass {.impl = self, .pc = pc},
        Passes::OpaqueSceneCopyPass {.impl = self}, Passes::ForwardPass {.impl = self}, Passes::HdrDenoisePass {.impl = self}, Passes::BloomPass {.impl = self}
    );

    // 2. Anti-aliasing tail. Which passes exist at all depends on the mode, so
    //    every AA setting is a different graph -- and therefore a different
    //    specialization of everything downstream, including which resource the
    //    tonemapper reads.
    auto aa = [&] {
        if constexpr (Mode == AAMode::TAA) {
            return Vk::MakePassPack(Passes::TAAPass {.impl = self});
        } else if constexpr (Mode == AAMode::FXAA) {
            return Vk::MakePassPack(Passes::FXAAPass {.impl = self});
        } else if constexpr (Mode == AAMode::MLAA) {
            return Vk::MakePassPack(Passes::MLAAPass {.impl = self});
        } else if constexpr (Mode == AAMode::SMAA) {
            return Vk::MakePassPack(Passes::SmaaEdgePass {.impl = self}, Passes::SmaaWeightPass {.impl = self}, Passes::SmaaBlendPass {.impl = self});
        } else {
            return Vk::MakePassPack();
        }
    }();

    // 3. Tone-mapping and the blit onto this frame's presentation image.
    auto blit = Vk::MakePassPack(
        Passes::TonemapBlitPass<Mode, std::decay_t<GetSwapchainImageT>> {.impl = self, .getSwapchainImage = std::forward<GetSwapchainImageT>(getSwapchain)}
    );

    return (std::move(core) + std::move(aa) + std::move(blit)).BuildGraph();
}

// The graph type for one anti-aliasing configuration. The visualization is a
// compile-time walk of the state table: no pass is constructed and no command
// buffer is involved, which is why `decltype` on an unevaluated call is enough.
template <AAMode Mode>
using SceneGraphFor = decltype(BuildFrameGraph<Mode>(
    std::declval<RenderContext::Impl&>(), std::declval<const ScenePushConstants&>(), std::declval<SwapchainImage (*)()>()
));

} // namespace ZHLN::RenderGraphDetail
