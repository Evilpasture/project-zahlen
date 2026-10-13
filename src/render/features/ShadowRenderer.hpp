// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "features/ShadowAtlas.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN {

// The shadow subsystem, collected in one place.
//
// Cascaded and atlas shadows are a multi-pass arrangement with state that is
// not one frame's business: a depth-only pipeline set that has to agree with the
// render targets' view masks, an indirect-command buffer the CPU fills and the
// GPU consumes, and the cascade fit that turns a camera frustum slice into a
// stable light-space matrix. None of that is a render pass -- it is what the
// passes are built out of -- so it lives here instead of being spread across
// `RenderContext::Impl`.
//
// The atlas pipeline is deliberately not multiview, unlike the cascades': a
// atlas tile is one draw with its own matrix in a push constant and its own
// viewport into a 2D depth image.
class ShadowRenderer {
  public:
    // The view mask that ties the cascade pipelines to the shadow map's layer
    // count: four cascades rendered in one multiview pass. `Passes::ShadowPass`
    // and the pipeline builder both read it, so the two cannot drift apart.
    static constexpr uint32_t kCascadeViewMask  = 0x0F;
    static constexpr float    kShadowClearDepth = 1.0f;

    // The indirect command buffer's shape: slot 0 is the cascades' draw list at
    // the scene-wide instance budget, then one slot per shadow-casting light. A
    // light's cube faces share its slot -- they see the same instances -- so
    // this is per light, not per tile.
    static constexpr uint32_t kAtlasInstancesPerLight = 512;

    ShadowRenderer()                                     = default;
    ~ShadowRenderer()                                    = default;
    ShadowRenderer(const ShadowRenderer&)                = delete;
    auto operator=(const ShadowRenderer&) -> ShadowRenderer& = delete;
    ShadowRenderer(ShadowRenderer&&) noexcept            = delete;
    auto operator=(ShadowRenderer&&) noexcept -> ShadowRenderer& = delete;

    // --- resources ----------------------------------------------------------

    // The CPU writes shadow commands while the GPU consumes earlier frames;
    // allocate one physical buffer for each in-flight frame slot.
    [[nodiscard]] auto InitResources(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;
    void DestroyResources(Vk::Allocator& allocator) noexcept;

    // --- pipelines ----------------------------------------------------------

    [[nodiscard]] auto CompileCascadePipelines(
        RenderContext::Impl& impl, VkDevice device, const Vk::ShaderDesc& vert, const Vk::ShaderDesc& frag
    ) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto CompileAtlasPipeline(
        RenderContext::Impl& impl, VkDevice device, const Vk::ShaderDesc& vert, const Vk::ShaderDesc& frag
    ) -> std::expected<void, ErrorCode>;

    // --- what the shadow pass needs -----------------------------------------

    [[nodiscard]] auto CascadePipeline() const noexcept -> VkPipeline;
    [[nodiscard]] auto CascadeLayout() const noexcept -> VkPipelineLayout;
    [[nodiscard]] auto CascadeMeshPipeline() const noexcept -> VkPipeline;
    [[nodiscard]] auto AtlasPipeline() const noexcept -> VkPipeline;
    [[nodiscard]] auto AtlasLayout() const noexcept -> VkPipelineLayout;

    [[nodiscard]] auto IndirectCommands(uint32_t frameIndex) noexcept -> Vk::Buffer&;
    [[nodiscard]] auto IndirectCommands(uint32_t frameIndex) const noexcept -> const Vk::Buffer&;

    // --- cascade math -------------------------------------------------------

    // Fits a bounding sphere around one frustum slice in light space and
    // snaps it to shadow-map texels, so a moving camera does not make the
    // cascade edges shimmer. `RenderContext::SetFrameData` calls this once per
    // cascade while packing the frame uniforms.
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
    VkPipelineLayout _cascadeLayout = VK_NULL_HANDLE;
    VkPipelineLayout _atlasLayout   = VK_NULL_HANDLE;

    Vk::TypedPipeline<0, true> _cascadePipeline;
    Vk::TypedPipeline<0, true> _atlasPipeline;
    Vk::TypedPipeline<0, true> _cascadeMeshPipeline;

    // `PerFrame` lives in `ZHLN`, next to `RenderContext::Impl` and the other
    // frame-lifetime ring helpers -- not in `ZHLN::Vk`, which owns the device
    // handles it wraps.
    PerFrame<Vk::Buffer> _indirectCommands;
};

}
