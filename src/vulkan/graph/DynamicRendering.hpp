// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../pipeline/PipelineTypes.hpp"
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace ZHLN::Vk {

static constexpr VkCommandBufferInheritanceInfo NullInheritanceInfo = {
    .sType                = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
    .pNext                = nullptr,
    .renderPass           = VK_NULL_HANDLE,
    .subpass              = 0,
    .framebuffer          = VK_NULL_HANDLE,
    .occlusionQueryEnable = VK_FALSE,
    .queryFlags           = 0,
    .pipelineStatistics   = 0
};

template <VkImageLayout Layout, VkFormat Format = VK_FORMAT_UNDEFINED>
struct TypedImage {
    static constexpr VkImageLayout layout       = Layout;
    static constexpr VkFormat      known_format = Format;
    ImageSlice                     slice {};

    constexpr TypedImage() noexcept = default;
    constexpr explicit TypedImage(ImageSlice s) noexcept: slice(s) {
    }

    [[nodiscard]] constexpr auto Handle() const noexcept -> VkImage {
        return slice.image;
    }

    [[nodiscard]] constexpr auto AttachmentView() const noexcept -> VkImageView {
        return slice.attachmentView;
    }
    [[nodiscard]] constexpr auto Extent() const noexcept -> VkExtent3D {
        return slice.extent;
    }
    [[nodiscard]] constexpr auto Extent2D() const noexcept -> VkExtent2D {
        return slice.Extent2D();
    }
    [[nodiscard]] constexpr auto GetFormat() const noexcept -> VkFormat {
        return slice.format;
    }
    [[nodiscard]] constexpr auto Aspect() const noexcept -> VkImageAspectFlags {
        return slice.aspect;
    }
    [[nodiscard]] constexpr auto Raw() const noexcept -> const ImageSlice& {
        return slice;
    }
    [[nodiscard]] constexpr auto operator->() const noexcept -> const ImageSlice* {
        return &slice;
    }
    template <VkImageLayout NewLayout>
    [[nodiscard]] constexpr auto WithLayout(VkImageAspectFlags newAspect) const noexcept -> TypedImage<NewLayout, Format> {
        ImageSlice result = slice;
        result.aspect     = newAspect;
        return TypedImage<NewLayout, Format> {result};
    }
};

template <VkImageLayout Layout, VkFormat Format>
    requires(Format != VK_FORMAT_UNDEFINED)
struct TypedImage<Layout, Format> {
    static constexpr VkImageLayout layout       = Layout;
    static constexpr VkFormat      known_format = Format;
    static constexpr VkFormat      format       = Format;
    const ImageSlice               slice;

  private:
    friend struct ImageSlice;
    template <VkImageLayout, VkFormat>
    friend struct TypedImage;
    constexpr explicit TypedImage(ImageSlice s) noexcept: slice(s) {
    }

  public:
    [[nodiscard]] constexpr auto Handle() const noexcept -> VkImage {
        return slice.image;
    }

    [[nodiscard]] constexpr auto AttachmentView() const noexcept -> VkImageView {
        return slice.attachmentView;
    }
    [[nodiscard]] constexpr auto Extent() const noexcept -> VkExtent3D {
        return slice.extent;
    }
    [[nodiscard]] constexpr auto Extent2D() const noexcept -> VkExtent2D {
        return slice.Extent2D();
    }
    [[nodiscard]] constexpr auto GetFormat() const noexcept -> VkFormat {
        return slice.format;
    }
    [[nodiscard]] constexpr auto Aspect() const noexcept -> VkImageAspectFlags {
        return slice.aspect;
    }
    [[nodiscard]] constexpr auto Raw() const noexcept -> const ImageSlice& {
        return slice;
    }
    [[nodiscard]] constexpr auto operator->() const noexcept -> const ImageSlice* {
        return &slice;
    }
    template <VkImageLayout NewLayout>
    [[nodiscard]] constexpr auto WithLayout(VkImageAspectFlags newAspect) const noexcept -> TypedImage<NewLayout, Format> {
        ImageSlice result = slice;
        result.aspect     = newAspect;
        return TypedImage<NewLayout, Format> {result};
    }
};

template <VkImageLayout Layout>
constexpr auto ImageSlice::Assume(VkImageAspectFlags imageAspect) const noexcept -> TypedImage<Layout, VK_FORMAT_UNDEFINED> {
    ImageSlice result = *this;
    result.aspect     = imageAspect;
    return TypedImage<Layout, VK_FORMAT_UNDEFINED> {result};
}

template <VkImageLayout Layout>
constexpr auto ImageSlice::Assume() const noexcept -> TypedImage<Layout, VK_FORMAT_UNDEFINED> {
    return Assume<Layout>(aspect);
}

template <VkFormat Format, VkImageLayout Layout>
constexpr auto ImageSlice::MatchFormat(VkImageAspectFlags imageAspect) const noexcept -> std::optional<TypedImage<Layout, Format>> {
    static_assert(Format != VK_FORMAT_UNDEFINED, "MatchFormat requires a concrete VkFormat.");
    if (!Valid() || format != Format) {
        return std::nullopt;
    }
    ImageSlice result = *this;
    result.aspect     = imageAspect;
    return TypedImage<Layout, Format> {result};
}

template <VkFormat Format, VkImageLayout Layout>
constexpr auto ImageSlice::MatchFormat() const noexcept -> std::optional<TypedImage<Layout, Format>> {
    return MatchFormat<Format, Layout>(aspect);
}

enum class AttachmentLayout : uint8_t {
    Undefined = 0,
    ColorAttachment,
    ShaderReadOnly,
    DepthStencilAttachment,
    TransferSrc,
    TransferDst,
};

[[nodiscard]] constexpr auto ToVkImageLayout(AttachmentLayout layout) noexcept -> VkImageLayout {
    switch (layout) {
        case AttachmentLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case AttachmentLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case AttachmentLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case AttachmentLayout::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case AttachmentLayout::TransferSrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case AttachmentLayout::TransferDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

static_assert(
    []() consteval {
        constexpr AttachmentLayout kEveryLayout[] = {
            AttachmentLayout::Undefined,   AttachmentLayout::ColorAttachment, AttachmentLayout::ShaderReadOnly, AttachmentLayout::DepthStencilAttachment,
            AttachmentLayout::TransferSrc, AttachmentLayout::TransferDst,
        };
        for (const AttachmentLayout layout: kEveryLayout) {
            if (ToVkImageLayout(layout) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                return false;
            }
        }
        return true;
    }(),
    "AttachmentLayout is the set of layouts a render target may be left in by a frame, and no pass may declare an image "
    "presentable: the presenter decides that from the swapchain, not from what a pass knows about its target."
);

struct UndefinedState {};
struct ColorAttachmentState {};
struct DepthAttachmentState {};
struct DepthStencilAttachmentState {};
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
struct LayoutMap<DepthStencilAttachmentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
};
template <>
struct LayoutMap<ShaderReadState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
template <>
struct LayoutMap<PresentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

template <typename T>
concept ColorClearTarget = std::same_as<std::remove_cvref_t<T>, VkImage> || requires(const T& target) {
    { target.image.Handle() } -> std::same_as<VkImage>;
} || requires(const T& target) {
    { target.Handle() } -> std::same_as<VkImage>;
} || requires(const T& target) {
    { target.image } -> std::convertible_to<VkImage>;
};

template <ColorClearTarget T>
[[nodiscard]] constexpr auto GetVkImage(const T& target) noexcept -> VkImage;

[[nodiscard]] constexpr auto ToVkClearColor(const Color4& color) noexcept -> VkClearColorValue;
[[nodiscard]] constexpr auto ToVkClearColor(float scalar) noexcept -> VkClearColorValue;
[[nodiscard]] constexpr auto ToVkClearColor(const VkClearColorValue& color) noexcept -> VkClearColorValue;

template <typename T>
concept ClearColorSource =
    std::same_as<std::remove_cvref_t<T>, Color4> || std::same_as<std::remove_cvref_t<T>, float> || std::same_as<std::remove_cvref_t<T>, VkClearColorValue>;

inline constexpr VkImageSubresourceRange kAllColorSubresources = {
    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel   = 0,
    .levelCount     = VK_REMAINING_MIP_LEVELS,
    .baseArrayLayer = 0,
    .layerCount     = VK_REMAINING_ARRAY_LAYERS,
};

template <
    VkImageLayout    InitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    VkImageLayout    FinalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    ColorClearTarget Target,
    ClearColorSource Color>
void ClearColorAndTransition(
    VkCommandBuffer                cmd,
    const Target&                  target,
    const Color&                   color,
    const VkImageSubresourceRange& range = kAllColorSubresources
) noexcept;

template <
    VkImageLayout    InitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    VkImageLayout    FinalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    ClearColorSource Color,
    ColorClearTarget... Targets>
    requires(sizeof...(Targets) > 1)
void ClearColorAndTransition(VkCommandBuffer cmd, const Color& color, const Targets&... targets) noexcept;

void ClearColorImage(VkCommandBuffer cmd, VkImage image, const VkClearColorValue& color, uint32_t layerCount = 1) noexcept;

template <typename InState, typename OutState, typename T>
auto IssueBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE);

template <VkImageLayout NewLayout, VkImageLayout OldLayout, VkFormat Format>
[[nodiscard]] auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, VkImageAspectFlags overrideAspect = VK_IMAGE_ASPECT_NONE) noexcept
    -> TypedImage<NewLayout, Format>;

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

static constexpr size_t kMaxColorAttachments = 8;

[[nodiscard]] constexpr auto StencilFormatForDepth(VkFormat depthFormat) noexcept -> VkFormat {
    return (depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || depthFormat == VK_FORMAT_D24_UNORM_S8_UINT) ? depthFormat : VK_FORMAT_UNDEFINED;
}

template <size_t ColorCount, bool HasDepth, typename Formats>
class DynamicPass;

// The viewport/scissor pair a pass applies to whatever it begins, primary or
// secondary. Both paths set it, from the same viewport, so it lives here once.
inline void ApplyViewportAndScissor(VkCommandBuffer cmd, VkViewport viewport) noexcept {
    const VkRect2D scissor = {
        .offset = {.x = static_cast<int32_t>(viewport.x), .y = static_cast<int32_t>(viewport.y)},
        .extent = {.width = static_cast<uint32_t>(viewport.width), .height = static_cast<uint32_t>(viewport.height)}
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

/// What a secondary command buffer inherits about the dynamic rendering it will
/// record into: the attachment formats, the view mask and the viewport.
///
/// A value, and a closed one: the formats are copied in at construction and the
/// struct the driver reads is produced by Link() on demand, so nothing outside
/// asks this object for a format and re-assembles the inheritance itself. A
/// default-constructed inheritance is inactive -- it links nothing -- which is
/// what a non-raster secondary (a compute fork) wants.
class RenderingInheritance {
    template <size_t, bool, typename>
    friend class DynamicPass;

  public:
    constexpr RenderingInheritance() noexcept = default;

    constexpr RenderingInheritance(
        std::span<const VkFormat> colorFormats, VkFormat depth, VkFormat stencil, uint32_t viewMask, VkViewport viewport
    ) noexcept:
        _colorFormatCount(static_cast<uint32_t>(std::min(colorFormats.size(), _colorFormats.size()))), _depthFormat(depth),
        _stencilFormat(stencil), _viewMask(viewMask), _viewport(viewport), _active(true) {
        for (size_t i = 0; i < _colorFormatCount; ++i) {
            _colorFormats[i] = colorFormats[i];
        }
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _active;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    /// Tells the inheritance chain to link this rendering state, and returns the
    /// head of the chain. `storage` must outlive the vkBeginCommandBuffer this
    /// feeds -- which SecondaryPassContext::Begin guarantees.
    [[nodiscard]] constexpr auto Link(void* pNext, VkCommandBufferInheritanceRenderingInfo& storage) const noexcept -> void* {
        if (!_active) {
            return pNext;
        }
        storage = {
            .sType                   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
            .pNext                   = pNext,
            .flags                   = 0,
            .viewMask                = _viewMask,
            .colorAttachmentCount    = _colorFormatCount,
            .pColorAttachmentFormats = _colorFormatCount > 0 ? _colorFormats.data() : nullptr,
            .depthAttachmentFormat   = _depthFormat,
            .stencilAttachmentFormat = _stencilFormat,
            .rasterizationSamples    = VK_SAMPLE_COUNT_1_BIT,
        };
        return &storage;
    }

    /// Tells the secondary command buffer to adopt the viewport this pass
    /// renders at; a non-raster inheritance has none to give.
    void ApplyViewportAndScissor(VkCommandBuffer cmd) const noexcept {
        if (_active) {
            ::ZHLN::Vk::ApplyViewportAndScissor(cmd, _viewport);
        }
    }

  private:
    std::array<VkFormat, kMaxColorAttachments> _colorFormats {};
    uint32_t                                   _colorFormatCount = 0;
    VkFormat                                   _depthFormat      = VK_FORMAT_UNDEFINED;
    VkFormat                                   _stencilFormat    = VK_FORMAT_UNDEFINED;
    uint32_t                                   _viewMask         = 0;
    VkViewport                                 _viewport {};
    bool                                       _active = false;
};

static_assert(!std::is_aggregate_v<RenderingInheritance>, "RenderingInheritance is a value with an invariant, not a field bag.");

template <VkImageLayout Layout>
struct Tag {};

inline constexpr Tag<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> AsColorAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> AsReadOnly;
inline constexpr Tag<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> AsDepthAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>          AsPresent;

template <VkImageLayout TargetLayout, VkImageLayout OldLayout, VkFormat Format>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, Tag<TargetLayout>) noexcept;

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

// -----------------------------------------------------------------------------
// Attachment Policies
// -----------------------------------------------------------------------------
/// The load/store pair, plus the value only a clear consumes, packaged with the
/// intent that produced it. A pass can no longer hand a clear color to a load.
struct ColorOp {
    VkAttachmentLoadOp  loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp storeOp    = VK_ATTACHMENT_STORE_OP_STORE;
    ZHLN::Color4        clearColor {};

    /// Clear the attachment with a color, then preserve results.
    [[nodiscard]] static constexpr auto Clear(ZHLN::Color4 color = {}) noexcept -> ColorOp {
        return {VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, color};
    }

    /// Load existing contents and store modified results (e.g., forward pass, blending).
    [[nodiscard]] static constexpr auto Load() noexcept -> ColorOp {
        return {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {}};
    }

    /// Do not load existing contents; every pixel will be overwritten (e.g., tonemapping blit).
    [[nodiscard]] static constexpr auto Overwrite() noexcept -> ColorOp {
        return {VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {}};
    }

    /// Clear, but discard results when the pass ends (transient scratch memory on tile GPUs).
    [[nodiscard]] static constexpr auto Transient(ZHLN::Color4 color = {}) noexcept -> ColorOp {
        return {VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, color};
    }
};

/// Depth half of the same idea. The clear value lives here and nowhere else, so a
/// float depth clear cannot reach a color attachment and vice versa.
struct DepthOp {
    VkAttachmentLoadOp  loadOp   = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp storeOp  = VK_ATTACHMENT_STORE_OP_STORE;
    float               clearVal = 1.0F;

    /// Clear depth to a specific value and write/store updates.
    [[nodiscard]] static constexpr auto Clear(float clearDepth = 1.0F) noexcept -> DepthOp {
        return {VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clearDepth};
    }

    /// Load previous depth and write/store updates (e.g., forward opaque pass).
    [[nodiscard]] static constexpr auto Load() noexcept -> DepthOp {
        return {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, 1.0F};
    }

    /// Test against previous depth without saving updates. STORE_OP_NONE (core in
    /// Vulkan 1.3) keeps the loaded contents intact instead of discarding them the
    /// way DONT_CARE would.
    [[nodiscard]] static constexpr auto ReadOnly() noexcept -> DepthOp {
        return {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_NONE, 1.0F};
    }

    /// Clear depth and discard on completion (transient depth buffer).
    [[nodiscard]] static constexpr auto Transient(float clearDepth = 1.0F) noexcept -> DepthOp {
        return {VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, clearDepth};
    }
};

template <size_t ColorCount = 0, bool HasDepth = false, typename Formats = AttachmentFormats<VK_FORMAT_UNDEFINED>>
class DynamicPass {
  public:
    constexpr explicit DynamicPass(VkExtent2D extent) noexcept
        requires(ColorCount == 0 && !HasDepth && std::same_as<Formats, AttachmentFormats<VK_FORMAT_UNDEFINED>>)
        : _extent(extent) {
    }

    constexpr explicit DynamicPass(VkExtent3D extent) noexcept
        requires(ColorCount == 0 && !HasDepth && std::same_as<Formats, AttachmentFormats<VK_FORMAT_UNDEFINED>>)
        : _extent({.width = extent.width, .height = extent.height}) {
    }

    constexpr auto Viewport(float x, float y, float width, float height) && -> DynamicPass<ColorCount, HasDepth, Formats>&& {
        _vpX = x;
        _vpY = y;
        _vpW = width;
        _vpH = height;
        return std::move(*this);
    }

    constexpr auto Viewport(const VkViewport& viewport) && -> DynamicPass<ColorCount, HasDepth, Formats>&& {
        return std::move(*this).Viewport(viewport.x, viewport.y, viewport.width, viewport.height);
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto AddColor(
        const TypedImage<Layout, Format>& img,
        VkAttachmentLoadOp                loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp    = VK_ATTACHMENT_STORE_OP_STORE,
        const ZHLN::Color4&               clearColor = {}
    ) && noexcept -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>;

    template <typename... TypedImages>
    constexpr auto AddColorGroup(
        const std::tuple<TypedImages...>& imageTuple,
        VkAttachmentLoadOp                loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp    = VK_ATTACHMENT_STORE_OP_STORE,
        const ZHLN::Color4&               clearColor = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
    ) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>;

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto AddDepth(
        const TypedImage<Layout, Format>& img,
        VkAttachmentLoadOp                loadOp   = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp  = VK_ATTACHMENT_STORE_OP_STORE,
        float                             clearVal = 1.0F
    ) && noexcept -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>;

    // -------------------------------------------------------------------------
    // Policy-Driven Attachments (Tier 2)
    // -------------------------------------------------------------------------
    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto AddColor(const TypedImage<Layout, Format>& img, ColorOp op) && noexcept
        -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>;

    template <typename... TypedImages>
    constexpr auto AddColorGroup(const std::tuple<TypedImages...>& imageTuple, ColorOp op) && noexcept
        -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>;

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto AddDepth(const TypedImage<Layout, Format>& img, DepthOp op) && noexcept
        -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>;

    // -------------------------------------------------------------------------
    // Semantic Intent Shorthands (Tier 1)
    // -------------------------------------------------------------------------
    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto ClearColor(const TypedImage<Layout, Format>& img, ZHLN::Color4 clearColor) && noexcept {
        return std::move(*this).AddColor(img, ColorOp::Clear(clearColor));
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto LoadColor(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddColor(img, ColorOp::Load());
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto OverwriteColor(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddColor(img, ColorOp::Overwrite());
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto ClearDepth(const TypedImage<Layout, Format>& img, float clearVal = 1.0F) && noexcept {
        return std::move(*this).AddDepth(img, DepthOp::Clear(clearVal));
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto LoadDepth(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddDepth(img, DepthOp::Load());
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto ReadOnlyDepth(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddDepth(img, DepthOp::ReadOnly());
    }

    constexpr auto Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&&;

    /// What a secondary command buffer must inherit to record this pass: the
    /// attachment formats, the view mask and the effective viewport. Nothing
    /// else -- descriptor heaps and frame rows are not this pass's to know.
    [[nodiscard]] constexpr auto GetInheritance() const noexcept -> RenderingInheritance;

    template <typename Func>
    void Execute(VkCommandBuffer cmd, Func&& func) const;

    constexpr auto ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&&;

    void Bind(VkCommandBuffer cmd, const TypedPipeline<ColorCount, HasDepth, Formats>& pipeline) const noexcept
        requires(!std::same_as<Formats, RuntimeAttachmentFormats>)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    }

    void Bind(VkCommandBuffer cmd, const TypedPipeline<ColorCount, HasDepth>& pipeline) const noexcept
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    }

  private:
    template <size_t C, bool D, typename F>
    friend class DynamicPass;

    template <size_t InsideCount, bool InsideDepth, typename InsideFormats>
    constexpr explicit DynamicPass(DynamicPass<InsideCount, InsideDepth, InsideFormats>&& other) noexcept:
        _extent(other._extent), _flags(other._flags), _colors(std::move(other)._colors), _colorFormats(other._colorFormats), _depth(other._depth),
        _depthFormat(other._depthFormat), _viewMask(other._viewMask), _hasStencil(other._hasStencil), _vpX(other._vpX), _vpY(other._vpY), _vpW(other._vpW),
        _vpH(other._vpH) {
    }

    [[nodiscard]] constexpr auto GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo*;
    [[nodiscard]] constexpr auto EffectiveViewport() const noexcept -> VkViewport;

    VkExtent2D                                                  _extent {};
    VkRenderingFlags                                            _flags = 0;
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> _colors {};
    std::array<VkFormat, kMaxColorAttachments>                  _colorFormats {};
    VkRenderingAttachmentInfo                                   _depth {};
    VkFormat                                                    _depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t                                                    _viewMask    = 0;
    bool                                                        _hasStencil  = false;
    float                                                       _vpX         = 0.0f;
    float                                                       _vpY         = 0.0f;
    float                                                       _vpW         = 0.0f;
    float                                                       _vpH         = 0.0f;
};

DynamicPass(VkExtent2D) -> DynamicPass<0, false>;

using PassRecordFn = void (*)(VkCommandBuffer, const void* userData);

struct PassDesc {
    const char*                    name = "Unnamed Pass";
    std::span<const ImageBarrier>  transitions;
    PassRecordFn                   record    = nullptr;
    const void*                    pUserData = nullptr;
};

void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept;

void ClearAttachment(
    VkCommandBuffer     cmd,
    VkImageAspectFlags  aspectMask,
    VkExtent2D          extent,
    const VkClearValue& value,
    uint32_t            baseArrayLayer = 0,
    uint32_t            layerCount     = 1
) noexcept;

} // namespace ZHLN::Vk

#include "DynamicRendering.inl"
