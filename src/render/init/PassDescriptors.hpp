// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../RenderInternal.hpp"
#include "pipeline/Postprocessing.hpp"
#include <initializer_list>
#include <span>

namespace ZHLN {

template <typename PassT>
struct GraphicsPassDesc {
    PassT&              pass;
    const char*         name {};
    VertexStageSource   vs;
    FragmentStageSource ps;
    VkFormat            colorFormat;
    bool                additive = false;
};

template <typename PassT>
GraphicsPassDesc(PassT&, const char*, VertexStageSource, FragmentStageSource, VkFormat, bool = false) -> GraphicsPassDesc<PassT>;

template <typename LayoutT>
[[nodiscard]] inline auto BuildPassHelper(
    RenderContext::Impl*            self,
    Vk::PostProcessPass<LayoutT>&   pass,
    VertexStageSource               vs,
    FragmentStageSource             ps,
    std::initializer_list<VkFormat> colorFormats,
    bool                            additive = false
) noexcept -> std::expected<void, ErrorCode> {
    return self->LoadAndCreateShaders(vs, ps).and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
        // VK_EXT_descriptor_heap: the pass is a heap pipeline (null layout,
        // PUSH_INDEX mapping table baked from the reflected set layout). Per-
        // draw data travels through push data, so no push ranges are declared.
        if (!pass.BuildHeap(
                self->ctx.Device(), self->heapManager, shaders, colorFormats, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame,
                additive, self->pipelineCache.Get()
            )) {
            return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
        }
        return {};
    });
}

template <typename LayoutT>
[[nodiscard]] inline auto BuildPassVariants(
    RenderContext::Impl*                  self,
    Vk::PostProcessPass<LayoutT>&         pass,
    VertexStageSource                     vs,
    FragmentStageSource                   ps,
    std::initializer_list<VkFormat>       colorFormats,
    std::span<const VkSpecializationInfo> specInfos,
    bool                                  additive = false
) noexcept -> std::expected<void, ErrorCode> {
    return self->LoadAndCreateShaders(vs, ps).and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
        // VK_EXT_descriptor_heap: specialization never changes the descriptor
        // interface, so one mapping table covers every variant.
        if (!pass.BuildHeapVariants(
                self->ctx.Device(), self->heapManager, shaders, colorFormats, specInfos, GpuAbi::kScenePushLayout.heapIndexOffset,
                Vk::HeapLifecycle::Frame, additive, self->pipelineCache.Get()
            )) {
            return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
        }
        return {};
    });
}

template <typename PassT>
[[nodiscard]] inline auto BuildDescribedPass(RenderContext::Impl* self, const GraphicsPassDesc<PassT>& desc) noexcept -> std::expected<void, ErrorCode> {
    auto result = BuildPassHelper(self, desc.pass, desc.vs, desc.ps, {desc.colorFormat}, desc.additive);
    if (!result) {
        return result;
    }
    self->RegisterShaderReload(
        desc.name, {desc.vs.path, desc.ps.path},
        [self, pass = &desc.pass, name = desc.name, vs = desc.vs, ps = desc.ps, fmt = desc.colorFormat, additive = desc.additive]() -> auto {
            auto reload = BuildPassHelper(self, *pass, vs, ps, {fmt}, additive);
            if (!reload) {
                ZHLN::Log("ERROR: Failed to hot-reload pipeline '{}': {}", name, reload.error());
            } else {
                ZHLN::Log("[Shader Reload] Pipeline '{}' hot-reloaded successfully.", name);
            }
        }
    );
    return {};
}

template <typename BuildFn>
[[nodiscard]] inline auto
    RegisterAndBuild(RenderContext::Impl* self, const char* name, BuildFn&& build_fn, std::initializer_list<const char*> watchPaths) noexcept
    -> std::expected<void, ErrorCode> {
    auto res = build_fn();
    if (!res) {
        return std::unexpected(res.error());
    }
    if constexpr (isDev) {
        self->RegisterShaderReload(name, watchPaths, [name, build_fn]() -> auto {
            auto reload_res = build_fn();
            if (!reload_res) {
                ZHLN::Log("ERROR: Failed to hot-reload pipeline '{}': {}", name, reload_res.error());
            } else {
                ZHLN::Log("[Shader Reload] Pipeline '{}' hot-reloaded successfully.", name);
            }
        });
    }
    return {};
}

} // namespace ZHLN
