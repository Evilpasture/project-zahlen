// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include "vulkan/VulkanHeader.hpp"

#include "vulkan/Rendering.hpp"
#include "render/features/ShadowAtlas.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN {

class ShadowRenderer {
  public:
    static constexpr uint32_t kCascadeViewMask  = 0x0F;
    static constexpr float    kShadowClearDepth = 1.0f;

    static constexpr uint32_t kAtlasInstancesPerLight = 512;

    ShadowRenderer()                                     = default;
    ~ShadowRenderer()                                    = default;
    ShadowRenderer(const ShadowRenderer&)                = delete;
    auto operator=(const ShadowRenderer&) -> ShadowRenderer& = delete;
    ShadowRenderer(ShadowRenderer&&) noexcept            = delete;
    auto operator=(ShadowRenderer&&) noexcept -> ShadowRenderer& = delete;

    [[nodiscard]] auto InitResources(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;
    void DestroyResources(Vk::Allocator& allocator) noexcept;

    [[nodiscard]] auto CompileCascadePipelines(
        RenderContext::Impl& impl, VkDevice device, const Vk::ShaderDesc& vert, const Vk::ShaderDesc& frag
    ) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto CompileAtlasPipeline(
        RenderContext::Impl& impl, VkDevice device, const Vk::ShaderDesc& vert, const Vk::ShaderDesc& frag
    ) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto CascadePipeline() const noexcept -> VkPipeline;
    [[nodiscard]] auto CascadeMeshPipeline() const noexcept -> VkPipeline;
    [[nodiscard]] auto AtlasPipeline() const noexcept -> VkPipeline;

    [[nodiscard]] auto IndirectCommands(uint32_t frameIndex) noexcept -> Vk::Buffer&;
    [[nodiscard]] auto IndirectCommands(uint32_t frameIndex) const noexcept -> const Vk::Buffer&;

    [[nodiscard]] static auto ComputeCascadeLightSpaceMatrix(
        const Camera&     cam,
        const Mat4x4& lightView,
        const FVec3&  sunDir,
        float             nearDist,
        float             farDist,
        float             aspect,
        float             tanHalfFov,
        uint32_t          shadowResolution
    ) noexcept -> Mat4x4;

  private:
    Vk::TypedPipeline<0, true> _cascadePipeline;
    Vk::TypedPipeline<0, true> _atlasPipeline;
    Vk::TypedPipeline<0, true> _cascadeMeshPipeline;

    PerFrame<Vk::Buffer> _indirectCommands;
};

}
