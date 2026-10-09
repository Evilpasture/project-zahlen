// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The frame pipeline, as a declaration.
//
// Nothing is recorded here. Each pass is a struct that knows which resources it
// touches and how to record itself, so this file's whole job is to list them in
// the right order and let the graph engine work out the hazards, the barriers
// and which ones can be replayed concurrently. A pass that needs frame state
// reads it from the `RenderContext::Impl&` it is handed at construction; a pass
// that needs per-frame tuning values carries them as members.

#include "RenderGraphBuild.hpp"
#include "Zahlen/Math3D.hpp"
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <utility>

namespace ZHLN {

namespace Vk {

template <>
struct ResourceResolver<Res_Depth> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_Depth>(impl.ActivePresentation().depthTarget);
    }
};

template <>
struct ResourceResolver<Res_ShadowMap> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_ShadowMap>(impl.graphResources.shadowMap);
    }
};

template <>
struct ResourceResolver<Res_TransLighting> {
    [[nodiscard]] static auto Resolve(RenderContext::Impl& impl) noexcept -> ImageSlice {
        const auto& target = impl.graphResources.transLightingTarget;
        // The graph uses mip 0 as a color attachment; the scene descriptor
        // separately samples the full mip chain (WriteTransLightingToHeap).
        return ImageSlice {target.image.Handle(), target.mipViews[0], target.extent, Res_TransLighting::format};
    }
};

template <>
struct ResourceResolver<Res_AccumPrevious> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_AccumPrevious>(impl.accumulationHistory.Previous());
    }
};

template <>
struct ResourceResolver<Res_AccumCurrent> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_AccumCurrent>(impl.accumulationHistory.Current());
    }
};

template <>
struct ResourceResolver<Res_Swapchain> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return *impl.sceneTarget;
    }
};

} // namespace Vk

namespace {

template <AAMode Mode, typename GetSwapchainImageT>
void ExecuteSceneGraph(
    RenderContext::Impl&    self,
    VkCommandBuffer         cmd,
    const SceneView&        view,
    const GraphicsSettings& settings,
    GetSwapchainImageT&&    getSwapchain
) {
    auto graph = RenderGraphDetail::BuildFrameGraph<Mode>(
        self, RenderGraphDetail::AssembleScenePushConstants(self, view, settings), std::forward<GetSwapchainImageT>(getSwapchain)
    );

    // Every resource a pass named is resolved once: explicit resolvers cover
    // non-bundle resources (swapchain and history) and the transmission
    // attachment's mip-0 view; the rest use reflected target metadata.
    typename decltype(graph)::Binder binder;
    binder.AutoBind(self);

    auto* diagnostics = self.gpuDiagnostics.IsActive() ? &self.gpuDiagnostics : nullptr;
    graph.Execute(cmd, binder, self.presenter.frameIndex, &self.gpuProfiler, diagnostics, self.ForkExecutor());
}

template <typename Self, typename GetSwapchainImageT>
void DispatchAAMode(Self& self, VkCommandBuffer cmd, AAMode mode, const SceneView& view, const GraphicsSettings& settings, GetSwapchainImageT&& getSwapchain) {
    Reflect::DispatchEnum(mode, [&]<AAMode Val>() { ExecuteSceneGraph<Val>(self, cmd, view, settings, std::forward<GetSwapchainImageT>(getSwapchain)); });
}

} // namespace

void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& sceneSettings) {
    auto getSwapchainImage = [&]() -> RenderGraphDetail::SwapchainImage { return sceneTarget->Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(); };

    DispatchAAMode(*this, cmd, sceneSettings.antiAliasing.mode, view, sceneSettings, getSwapchainImage);
}

} // namespace ZHLN
