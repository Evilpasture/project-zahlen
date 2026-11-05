// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D pass execution -- the Vulkan-side verbs a graph pass body
// records against. The declaration vocabulary (the RenderPass base, the
// Usage hazards) is neutral and lives in <vkd3d/graph/PassDecl.hpp>; this
// header is the Vulkan direction of it, matching how
// <vkd3d/VulkanBarriers.hpp> defines Record() for VkCommandBuffer.

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/FunctionRef.hpp>
#include <concepts>
#include <span>

namespace ZHLN::Vk {

using ForkCall = ZHLN::FunctionRef<void(VkCommandBuffer) const>;

template <typename Executor>
concept ForkRecorder = requires(Executor& executor, VkCommandBuffer cmd, std::span<const ForkCall> bodies) {
    { executor.ExecuteFork(cmd, bodies) } noexcept;
};

struct SequentialFork {
    static constexpr void ExecuteFork(VkCommandBuffer cmd, std::span<const ForkCall> bodies) noexcept {
        for (const ForkCall& body : bodies) {
            body(cmd);
        }
    }
};

static_assert(ForkRecorder<SequentialFork>);

class RasterPassContextBase {
  public:
    explicit RasterPassContextBase(VkCommandBuffer cmd) noexcept: m_cmd(cmd) {
    }
    ~RasterPassContextBase() = default;

    RasterPassContextBase(const RasterPassContextBase&)                        = delete;
    auto operator=(const RasterPassContextBase&) -> RasterPassContextBase&     = delete;
    RasterPassContextBase(RasterPassContextBase&&) noexcept                    = delete;
    auto operator=(RasterPassContextBase&&) noexcept -> RasterPassContextBase& = delete;

    [[nodiscard]] auto Cmd() const noexcept -> VkCommandBuffer {
        return m_cmd;
    }
    [[nodiscard]] auto Extent() const noexcept -> VkExtent2D {
        return m_extent;
    }

    // The context opened rendering at the full attachment extent; a pass
    // whose domain viewport differs (the scene viewport) says so, and the
    // context applies viewport and matching scissor itself.
    void SetViewport(const VkViewport& viewport) noexcept {
        const VkRect2D scissor = {
            .offset = {.x = static_cast<int32_t>(viewport.x), .y = static_cast<int32_t>(viewport.y)},
            .extent = {.width = static_cast<uint32_t>(viewport.width), .height = static_cast<uint32_t>(viewport.height)},
        };
        vkCmdSetViewport(m_cmd, 0, 1, &viewport);
        vkCmdSetScissor(m_cmd, 0, 1, &scissor);
    }

  protected:
    void SetExtent(VkExtent2D extent) noexcept {
        m_extent = extent;
    }

  private:
    VkCommandBuffer m_cmd;
    VkExtent2D      m_extent {};
};

} // namespace ZHLN::Vk
