// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

static constexpr VkCommandBufferInheritanceInfo NullInheritanceInfo = {
    .sType                = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
    .pNext                = nullptr,
    .renderPass           = VK_NULL_HANDLE, // Ignored in Dynamic Rendering
    .subpass              = 0,
    .framebuffer          = VK_NULL_HANDLE,
    .occlusionQueryEnable = VK_FALSE,
    .queryFlags           = 0,
    .pipelineStatistics   = 0
};

/**
 * @brief Zero-overhead, compile-time layout tracker.
 * Memory footprint is identical to passing raw handles, but the compiler enforces state.
 */
template <VkImageLayout Layout>
struct TypedImage {
    static constexpr VkImageLayout layout = Layout;
    VkImage                        handle = VK_NULL_HANDLE;
    VkImageView                    view   = VK_NULL_HANDLE;
    VkExtent3D                     extent {}; // Modified from VkExtent2D
    VkImageAspectFlags             aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormat                       format = VK_FORMAT_UNDEFINED;
};

// ============================================================================
// Compile-Time Layout State Contract
// ============================================================================

struct UndefinedState {};
struct ColorAttachmentState {};
struct DepthAttachmentState {};
struct ShaderReadState {};
struct PresentState {};

template <typename State>
struct LayoutMap;

template <>
struct LayoutMap<UndefinedState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_UNDEFINED;
};
template <>
struct LayoutMap<ColorAttachmentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};
template <>
struct LayoutMap<DepthAttachmentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
};
template <>
struct LayoutMap<ShaderReadState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
template <>
struct LayoutMap<PresentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

template <VkImageLayout Layout>
struct LayoutTraits;

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
void TransitionLayout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept;

template <typename InState, typename OutState, typename T>
auto IssueBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE);

template <VkImageLayout NewLayout, VkImageLayout OldLayout>
[[nodiscard]] auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img, VkImageAspectFlags overrideAspect = VK_IMAGE_ASPECT_NONE) noexcept
    -> TypedImage<NewLayout>;

// ============================================================================
// Scoped RAII Layout Transition Guards
// ============================================================================

template <typename SrcState, typename DstState>
class ScopedBarrierGuard {
  public:
    VkCommandBuffer                        cmd;
    TypedImage<LayoutMap<SrcState>::value> resource;
    VkImageAspectFlags                     aspectOverride;
    bool                                   active = true;

    ScopedBarrierGuard(VkCommandBuffer c, const TypedImage<LayoutMap<SrcState>::value>& res, VkImageAspectFlags aspect) noexcept;
    ~ScopedBarrierGuard() noexcept;

    ScopedBarrierGuard(const ScopedBarrierGuard&)                    = delete;
    auto operator=(const ScopedBarrierGuard&) -> ScopedBarrierGuard& = delete;

    ScopedBarrierGuard(ScopedBarrierGuard&& other) noexcept;
    auto operator=(ScopedBarrierGuard&& other) noexcept -> ScopedBarrierGuard&;
};

template <typename SrcState, typename DstState, typename T>
[[nodiscard]] auto ScopedBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE) noexcept;

// ============================================================================
// Scoped Barrier Functor (Customization Point Objects)
// ============================================================================

template <typename SrcState, typename DstState>
struct ScopedBarrierTrans {
    template <typename T>
    [[nodiscard]] auto operator()(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE) const noexcept {
        return ScopedBarrier<SrcState, DstState, T>(cmd, resource, aspectOverride);
    }
};

using ReadToColorTrans = ScopedBarrierTrans<Vk::ShaderReadState, Vk::ColorAttachmentState>;
using ColorToReadTrans = ScopedBarrierTrans<Vk::ColorAttachmentState, Vk::ShaderReadState>;

inline constexpr ReadToColorTrans ReadToColor {};
inline constexpr ColorToReadTrans ColorToRead {};

// ============================================================================
// Dynamic Render Pass Builder
// ============================================================================

static constexpr size_t kMaxColorAttachments = 8;

template <VkImageLayout Layout>
struct Tag {};

inline constexpr Tag<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> AsColorAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> AsReadOnly;
inline constexpr Tag<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> AsDepthAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>          AsPresent;

template <VkImageLayout TargetLayout, VkImageLayout OldLayout>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img, Tag<TargetLayout> /*unused*/) noexcept;

template <typename ImageT, VkImageLayout Final>
class ScopedTransition {
  public:
    ScopedTransition(VkCommandBuffer cmd, ImageT& image, Vk::Tag<Final> transitionTag): cmd_(cmd), image_(Transition(cmd, image, transitionTag)) {
    }
    ~ScopedTransition() {
        [[maybe_unused]] auto _ = Transition(cmd_, image_, Vk::AsReadOnly);
    }
    ScopedTransition(const ScopedTransition&)            = delete;
    ScopedTransition& operator=(const ScopedTransition&) = delete;
    ScopedTransition(ScopedTransition&&)                 = delete;
    ScopedTransition& operator=(ScopedTransition&&)      = delete;
    auto&             Get() {
        return image_;
    }

  private:
    VkCommandBuffer       cmd_;
    Vk::TypedImage<Final> image_;
};

template <typename ImageT, VkImageLayout Final>
ScopedTransition(VkCommandBuffer, ImageT&, Vk::Tag<Final>) -> ScopedTransition<ImageT, Final>;

template <size_t ColorCount = 0, bool HasDepth = false>
class DynamicPass {
  public:
    // 2D Constructor
    constexpr explicit DynamicPass(VkExtent2D extent) noexcept: _extent(extent) {
    }

    // 3D Constructor (Safely isolates depth)
    constexpr explicit DynamicPass(VkExtent3D extent) noexcept: _extent({.width = extent.width, .height = extent.height}) {
    }

    template <size_t InsideCount, bool InsideDepth>
    constexpr explicit DynamicPass(DynamicPass<InsideCount, InsideDepth>&& other) noexcept:
        _extent(other._extent), _flags(other._flags), _colors(std::move(other)._colors), _depth(other._depth), _viewMask(other._viewMask) {
    }

    template <VkImageLayout Layout>
    constexpr auto AddColor(
        const TypedImage<Layout>& img,
        VkAttachmentLoadOp        loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp       storeOp    = VK_ATTACHMENT_STORE_OP_STORE,
        const ZHLN::Color4&       clearColor = {}
    ) && noexcept -> DynamicPass<ColorCount + 1, HasDepth>;

    template <typename... TypedImages>
    constexpr auto AddColorGroup(
        const std::tuple<TypedImages...>& imageTuple,
        VkAttachmentLoadOp                loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp    = VK_ATTACHMENT_STORE_OP_STORE,
        const ZHLN::Color4&               clearColor = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
    ) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth>;

    template <VkImageLayout Layout>
    constexpr auto AddDepth(
        const TypedImage<Layout>& img,
        VkAttachmentLoadOp        loadOp   = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp       storeOp  = VK_ATTACHMENT_STORE_OP_STORE,
        float                     clearVal = 1.0F
    ) && noexcept -> DynamicPass<ColorCount, true>;

    constexpr auto Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth>&&;

    template <typename Func>
    void Execute(VkCommandBuffer cmd, Func&& func) const;

    constexpr auto ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth>&&;

    void Bind(VkCommandBuffer cmd, const TypedPipeline<ColorCount, HasDepth>& pipeline) const noexcept {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    }

  private:
    template <size_t C, bool D>
    friend class DynamicPass;

    [[nodiscard]] constexpr auto GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo*;

    VkExtent2D                                                  _extent {};
    VkRenderingFlags                                            _flags = 0;
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> _colors {};
    VkRenderingAttachmentInfo                                   _depth {};
    uint32_t                                                    _viewMask = 0;
};

DynamicPass(VkExtent2D) -> DynamicPass<0, false>;

// ============================================================================
// Zero-Allocation Render Graph Structs
// ============================================================================

struct PassResource {
    ZHLN_ImageBarrierDesc barrier;
};

using PassRecordFn = void (*)(VkCommandBuffer, const void* userData);

struct PassDesc {
    const char*                   name = "Unnamed Pass";
    std::span<const PassResource> transitions;
    PassRecordFn                  record    = nullptr;
    const void*                   pUserData = nullptr;
};

template <size_t MaxStackBarriers = 16>
void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept;

} // namespace ZHLN::Vk

#include "DynamicRendering.inl"
