// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "features/ShadowAtlas.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Math/Math3D.hpp>
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
        const JPH::Mat44& lightView,
        const JPH::Vec3&  sunDir,
        float             nearDist,
        float             farDist,
        float             aspect,
        float             tanHalfFov,
        uint32_t          shadowResolution
    ) noexcept -> JPH::Mat44;

  private:
    Vk::TypedPipeline<0, true> _cascadePipeline;
    Vk::TypedPipeline<0, true> _atlasPipeline;
    Vk::TypedPipeline<0, true> _cascadeMeshPipeline;

    PerFrame<Vk::Buffer> _indirectCommands;
};

}
