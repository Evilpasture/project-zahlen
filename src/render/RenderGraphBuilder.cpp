// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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
        return MakeRef<Res_Depth>(impl.ActiveDepthTarget());
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

        return ImageSlice {target.image.Handle(), target.mipAttachmentViews[0], target.extent, Res_TransLighting::format};
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

    typename decltype(graph)::Binder binder;
    binder.AutoBind(self);

    auto* diagnostics = self.diagnostics.gpuDiagnostics.IsActive() ? &self.diagnostics.gpuDiagnostics : nullptr;
    graph.Execute(cmd, binder, self.presenter.frameIndex, &self.diagnostics.gpuProfiler, diagnostics, self.ForkExecutor());
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
