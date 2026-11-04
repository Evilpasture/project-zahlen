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

template <Vk::ImageLayout Layout, Vk::Format Format = Vk::Format::Undefined>
struct TypedImage {
    static constexpr Vk::ImageLayout layout       = Layout;
    static constexpr Vk::Format      known_format = Format;
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
    [[nodiscard]] constexpr auto GetFormat() const noexcept -> Vk::Format {
        return slice.format;
    }
    [[nodiscard]] constexpr auto Aspect() const noexcept -> Vk::ImageAspect {
        return slice.aspect;
    }
    [[nodiscard]] constexpr auto Raw() const noexcept -> const ImageSlice& {
        return slice;
    }
    [[nodiscard]] constexpr auto operator->() const noexcept -> const ImageSlice* {
        return &slice;
    }
    template <Vk::ImageLayout NewLayout>
    [[nodiscard]] constexpr auto WithLayout(Vk::ImageAspect newAspect) const noexcept -> TypedImage<NewLayout, Format> {
        ImageSlice result = slice;
        result.aspect     = newAspect;
        return TypedImage<NewLayout, Format> {result};
    }
};

template <Vk::ImageLayout Layout, Vk::Format Format>
    requires(Format != Vk::Format::Undefined)
struct TypedImage<Layout, Format> {
    static constexpr Vk::ImageLayout layout       = Layout;
    static constexpr Vk::Format      known_format = Format;
    static constexpr Vk::Format      format       = Format;
    const ImageSlice               slice;

  private:
    friend struct ImageSlice;
    template <Vk::ImageLayout, Vk::Format>
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
    [[nodiscard]] constexpr auto GetFormat() const noexcept -> Vk::Format {
        return slice.format;
    }
    [[nodiscard]] constexpr auto Aspect() const noexcept -> Vk::ImageAspect {
        return slice.aspect;
    }
    [[nodiscard]] constexpr auto Raw() const noexcept -> const ImageSlice& {
        return slice;
    }
    [[nodiscard]] constexpr auto operator->() const noexcept -> const ImageSlice* {
        return &slice;
    }
    template <Vk::ImageLayout NewLayout>
    [[nodiscard]] constexpr auto WithLayout(Vk::ImageAspect newAspect) const noexcept -> TypedImage<NewLayout, Format> {
        ImageSlice result = slice;
        result.aspect     = newAspect;
        return TypedImage<NewLayout, Format> {result};
    }
};

template <Vk::ImageLayout Layout>
constexpr auto ImageSlice::Assume(Vk::ImageAspect imageAspect) const noexcept -> TypedImage<Layout, Vk::Format::Undefined> {
    ImageSlice result = *this;
    result.aspect     = imageAspect;
    return TypedImage<Layout, Vk::Format::Undefined> {result};
}

template <Vk::ImageLayout Layout>
constexpr auto ImageSlice::Assume() const noexcept -> TypedImage<Layout, Vk::Format::Undefined> {
    return Assume<Layout>(aspect);
}

template <Vk::Format Format, Vk::ImageLayout Layout>
constexpr auto ImageSlice::MatchFormat(Vk::ImageAspect imageAspect) const noexcept -> std::optional<TypedImage<Layout, Format>> {
    static_assert(Format != Vk::Format::Undefined, "MatchFormat requires a concrete Vk::Format.");
    if (!Valid() || format != Format) {
        return std::nullopt;
    }
    ImageSlice result = *this;
    result.aspect     = imageAspect;
    return TypedImage<Layout, Format> {result};
}

template <Vk::Format Format, Vk::ImageLayout Layout>
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

[[nodiscard]] constexpr auto ToVkImageLayout(AttachmentLayout layout) noexcept -> Vk::ImageLayout {
    switch (layout) {
        case AttachmentLayout::Undefined:
            return Vk::ImageLayout::Undefined;
        case AttachmentLayout::ColorAttachment:
            return Vk::ImageLayout::ColorAttachmentOptimal;
        case AttachmentLayout::ShaderReadOnly:
            return Vk::ImageLayout::ShaderReadOnlyOptimal;
        case AttachmentLayout::DepthStencilAttachment:
            return Vk::ImageLayout::DepthStencilAttachmentOptimal;
        case AttachmentLayout::TransferSrc:
            return Vk::ImageLayout::TransferSrcOptimal;
        case AttachmentLayout::TransferDst:
            return Vk::ImageLayout::TransferDstOptimal;
    }
    return Vk::ImageLayout::Undefined;
}

static_assert(
    []() consteval {
        constexpr AttachmentLayout kEveryLayout[] = {
            AttachmentLayout::Undefined,   AttachmentLayout::ColorAttachment, AttachmentLayout::ShaderReadOnly, AttachmentLayout::DepthStencilAttachment,
            AttachmentLayout::TransferSrc, AttachmentLayout::TransferDst,
        };
        for (const AttachmentLayout layout: kEveryLayout) {
            if (ToVkImageLayout(layout) == Vk::ImageLayout::PresentSrc) {
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
    static constexpr Vk::ImageLayout value = Vk::ImageLayout::Undefined;
};
template <>
struct LayoutMap<ColorAttachmentState> {
    static constexpr Vk::ImageLayout value = Vk::ImageLayout::ColorAttachmentOptimal;
};
template <>
struct LayoutMap<DepthAttachmentState> {
    static constexpr Vk::ImageLayout value = Vk::ImageLayout::DepthAttachmentOptimal;
};
template <>
struct LayoutMap<DepthStencilAttachmentState> {
    static constexpr Vk::ImageLayout value = Vk::ImageLayout::DepthStencilAttachmentOptimal;
};
template <>
struct LayoutMap<ShaderReadState> {
    static constexpr Vk::ImageLayout value = Vk::ImageLayout::ShaderReadOnlyOptimal;
};
template <>
struct LayoutMap<PresentState> {
    static constexpr Vk::ImageLayout value = Vk::ImageLayout::PresentSrc;
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

[[nodiscard]] constexpr auto ToVkClearColor(ZHLN::Vk::Color4 color) noexcept -> VkClearColorValue;
[[nodiscard]] constexpr auto ToVkClearColor(float scalar) noexcept -> VkClearColorValue;
[[nodiscard]] constexpr auto ToVkClearColor(VkClearColorValue color) noexcept -> VkClearColorValue;

template <typename T>
concept ClearColorSource =
    std::same_as<std::remove_cvref_t<T>, Color4> || std::same_as<std::remove_cvref_t<T>, float> || std::same_as<std::remove_cvref_t<T>, VkClearColorValue>;

inline constexpr VkImageSubresourceRange kAllColorSubresources = {
    .aspectMask     = ToVk(Vk::ImageAspect::Color),
    .baseMipLevel   = 0,
    .levelCount     = VK_REMAINING_MIP_LEVELS,
    .baseArrayLayer = 0,
    .layerCount     = VK_REMAINING_ARRAY_LAYERS,
};

template <
    Vk::ImageLayout    InitialLayout = Vk::ImageLayout::Undefined,
    Vk::ImageLayout    FinalLayout   = Vk::ImageLayout::ShaderReadOnlyOptimal,
    ColorClearTarget Target,
    ClearColorSource Color>
void ClearColorAndTransition(VkCommandBuffer cmd, const Target& target, const Color& color, VkImageSubresourceRange range = kAllColorSubresources) noexcept;

template <
    Vk::ImageLayout    InitialLayout = Vk::ImageLayout::Undefined,
    Vk::ImageLayout    FinalLayout   = Vk::ImageLayout::ShaderReadOnlyOptimal,
    ClearColorSource Color,
    ColorClearTarget... Targets>
    requires(sizeof...(Targets) > 1)
void ClearColorAndTransition(VkCommandBuffer cmd, const Color& color, const Targets&... targets) noexcept;

void ClearColorImage(VkCommandBuffer cmd, VkImage image, VkClearColorValue color, uint32_t layerCount = 1) noexcept;

template <typename InState, typename OutState, typename T>
auto IssueBarrier(VkCommandBuffer cmd, const T& resource, Vk::ImageAspect aspectOverride = Vk::ImageAspect::None);

template <Vk::ImageLayout NewLayout, Vk::ImageLayout OldLayout, Vk::Format Format>
[[nodiscard]] auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, Vk::ImageAspect overrideAspect = Vk::ImageAspect::None) noexcept
    -> TypedImage<NewLayout, Format>;

template <typename SrcState, typename DstState>
class ScopedBarrierGuard {
  public:
    VkCommandBuffer                        cmd;
    TypedImage<LayoutMap<SrcState>::value> resource;
    Vk::ImageAspect                     aspectOverride;
    bool                                   active = true;

    ScopedBarrierGuard(VkCommandBuffer c, const TypedImage<LayoutMap<SrcState>::value>& res, Vk::ImageAspect aspect) noexcept;
    ~ScopedBarrierGuard() noexcept;

    ScopedBarrierGuard(const ScopedBarrierGuard&)                    = delete;
    auto operator=(const ScopedBarrierGuard&) -> ScopedBarrierGuard& = delete;

    ScopedBarrierGuard(ScopedBarrierGuard&& other) noexcept;
    auto operator=(ScopedBarrierGuard&& other) noexcept -> ScopedBarrierGuard&;
};

template <typename SrcState, typename DstState, typename T>
[[nodiscard]] auto ScopedBarrier(VkCommandBuffer cmd, const T& resource, Vk::ImageAspect aspectOverride = Vk::ImageAspect::None) noexcept;

template <typename SrcState, typename DstState>
struct ScopedBarrierTrans {
    template <typename T>
    [[nodiscard]] auto operator()(VkCommandBuffer cmd, const T& resource, Vk::ImageAspect aspectOverride = Vk::ImageAspect::None) const noexcept {
        return ScopedBarrier<SrcState, DstState, T>(cmd, resource, aspectOverride);
    }
};

using ReadToColorTrans = ScopedBarrierTrans<Vk::ShaderReadState, Vk::ColorAttachmentState>;
using ColorToReadTrans = ScopedBarrierTrans<Vk::ColorAttachmentState, Vk::ShaderReadState>;

inline constexpr ReadToColorTrans ReadToColor {};
inline constexpr ColorToReadTrans ColorToRead {};

static constexpr size_t kMaxColorAttachments = 8;

[[nodiscard]] constexpr auto StencilFormatForDepth(Vk::Format depthFormat) noexcept -> Vk::Format {
    return (depthFormat == Vk::Format::D32SfloatS8Uint || depthFormat == Vk::Format::D24UnormS8Uint) ? depthFormat : Vk::Format::Undefined;
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

    constexpr RenderingInheritance(std::span<const VkFormat> colorFormats, VkFormat depth, VkFormat stencil, uint32_t viewMask, VkViewport viewport) noexcept:
        _colorFormatCount(static_cast<uint32_t>(std::min(colorFormats.size(), _colorFormats.size()))), _depthFormat(depth), _stencilFormat(stencil),
        _viewMask(viewMask), _viewport(viewport), _active(true) {
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
            Vk::ApplyViewportAndScissor(cmd, _viewport);
        }
    }

  private:
    // Stored raw: Link() points the driver struct at this array, and the
    // array must outlive Link() in this object (copied in as native formats).
    std::array<VkFormat, kMaxColorAttachments> _colorFormats {};
    uint32_t                                   _colorFormatCount = 0;
    VkFormat                                   _depthFormat      = VK_FORMAT_UNDEFINED;
    VkFormat                                   _stencilFormat    = VK_FORMAT_UNDEFINED;
    uint32_t                                   _viewMask         = 0;
    VkViewport                                 _viewport {};
    bool                                       _active = false;
};

static_assert(!std::is_aggregate_v<RenderingInheritance>, "RenderingInheritance is a value with an invariant, not a field bag.");

template <Vk::ImageLayout Layout>
struct Tag {};

inline constexpr Tag<Vk::ImageLayout::ColorAttachmentOptimal> AsColorAttachment;
inline constexpr Tag<Vk::ImageLayout::ShaderReadOnlyOptimal> AsReadOnly;
inline constexpr Tag<Vk::ImageLayout::DepthAttachmentOptimal> AsDepthAttachment;
inline constexpr Tag<Vk::ImageLayout::PresentSrc>          AsPresent;

template <Vk::ImageLayout TargetLayout, Vk::ImageLayout OldLayout, Vk::Format Format>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, Tag<TargetLayout>) noexcept;

template <typename ImageT, Vk::ImageLayout Final>
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

template <typename ImageT, Vk::ImageLayout Final>
ScopedTransition(VkCommandBuffer, ImageT&, Vk::Tag<Final>) -> ScopedTransition<ImageT, Final>;

// -----------------------------------------------------------------------------
// Attachment Policies
// -----------------------------------------------------------------------------
/// The load/store pair, plus the value only a clear consumes, packaged with the
/// intent that produced it. A pass can no longer hand a clear color to a load.
struct ColorOp {
    Vk::AttachmentLoadOp  loadOp  = Vk::AttachmentLoadOp::Load;
    Vk::AttachmentStoreOp storeOp = Vk::AttachmentStoreOp::Store;
    ZHLN::Color4        clearColor {};

    /// Clear the attachment with a color, then preserve results.
    [[nodiscard]] static constexpr auto Clear(ZHLN::Color4 color = {}) noexcept -> ColorOp {
        return {Vk::AttachmentLoadOp::Clear, Vk::AttachmentStoreOp::Store, color};
    }

    /// Load existing contents and store modified results (e.g., forward pass, blending).
    [[nodiscard]] static constexpr auto Load() noexcept -> ColorOp {
        return {Vk::AttachmentLoadOp::Load, Vk::AttachmentStoreOp::Store, {}};
    }

    /// Do not load existing contents; every pixel will be overwritten (e.g., tonemapping blit).
    [[nodiscard]] static constexpr auto Overwrite() noexcept -> ColorOp {
        return {Vk::AttachmentLoadOp::DontCare, Vk::AttachmentStoreOp::Store, {}};
    }

    /// Clear, but discard results when the pass ends (transient scratch memory on tile GPUs).
    [[nodiscard]] static constexpr auto Transient(ZHLN::Color4 color = {}) noexcept -> ColorOp {
        return {Vk::AttachmentLoadOp::Clear, Vk::AttachmentStoreOp::DontCare, color};
    }
};

/// Depth half of the same idea. The clear value lives here and nowhere else, so a
/// float depth clear cannot reach a color attachment and vice versa.
struct DepthOp {
    Vk::AttachmentLoadOp  loadOp   = Vk::AttachmentLoadOp::Load;
    Vk::AttachmentStoreOp storeOp  = Vk::AttachmentStoreOp::Store;
    float               clearVal = 1.0F;

    /// Clear depth to a specific value and write/store updates.
    [[nodiscard]] static constexpr auto Clear(float clearDepth = 1.0F) noexcept -> DepthOp {
        return {Vk::AttachmentLoadOp::Clear, Vk::AttachmentStoreOp::Store, clearDepth};
    }

    /// Load previous depth and write/store updates (e.g., forward opaque pass).
    [[nodiscard]] static constexpr auto Load() noexcept -> DepthOp {
        return {Vk::AttachmentLoadOp::Load, Vk::AttachmentStoreOp::Store, 1.0F};
    }

    /// Test against previous depth without saving updates. STORE_OP_NONE (core in
    /// Vulkan 1.3) keeps the loaded contents intact instead of discarding them the
    /// way DONT_CARE would.
    [[nodiscard]] static constexpr auto ReadOnly() noexcept -> DepthOp {
        return {Vk::AttachmentLoadOp::Load, Vk::AttachmentStoreOp::None, 1.0F};
    }

    /// Clear depth and discard on completion (transient depth buffer).
    [[nodiscard]] static constexpr auto Transient(float clearDepth = 1.0F) noexcept -> DepthOp {
        return {Vk::AttachmentLoadOp::Clear, Vk::AttachmentStoreOp::DontCare, clearDepth};
    }
};

template <size_t ColorCount = 0, bool HasDepth = false, typename Formats = AttachmentFormats<Vk::Format::Undefined>>
class DynamicPass {
  public:
    constexpr explicit DynamicPass(VkExtent2D extent) noexcept
        requires(ColorCount == 0 && !HasDepth && std::same_as<Formats, AttachmentFormats<Vk::Format::Undefined>>)
        : _extent(extent) {
    }

    constexpr explicit DynamicPass(VkExtent3D extent) noexcept
        requires(ColorCount == 0 && !HasDepth && std::same_as<Formats, AttachmentFormats<Vk::Format::Undefined>>)
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

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto AddColor(
        const TypedImage<Layout, Format>& img,
        Vk::AttachmentLoadOp                loadOp     = Vk::AttachmentLoadOp::Load,
        Vk::AttachmentStoreOp               storeOp    = Vk::AttachmentStoreOp::Store,
        ZHLN::Vk::Color4                  clearColor = {}
    ) && noexcept -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>;

    template <typename... TypedImages>
    constexpr auto AddColorGroup(
        const std::tuple<TypedImages...>& imageTuple,
        Vk::AttachmentLoadOp                loadOp     = Vk::AttachmentLoadOp::Load,
        Vk::AttachmentStoreOp               storeOp    = Vk::AttachmentStoreOp::Store,
        ZHLN::Vk::Color4                  clearColor = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
    ) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>;

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto AddDepth(
        const TypedImage<Layout, Format>& img,
        Vk::AttachmentLoadOp                loadOp   = Vk::AttachmentLoadOp::Load,
        Vk::AttachmentStoreOp               storeOp  = Vk::AttachmentStoreOp::Store,
        float                             clearVal = 1.0F
    ) && noexcept -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>;

    // -------------------------------------------------------------------------
    // Policy-Driven Attachments (Tier 2)
    // -------------------------------------------------------------------------
    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto AddColor(const TypedImage<Layout, Format>& img, ColorOp op) && noexcept
        -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>;

    template <typename... TypedImages>
    constexpr auto AddColorGroup(const std::tuple<TypedImages...>& imageTuple, ColorOp op) && noexcept
        -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>;

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto AddDepth(const TypedImage<Layout, Format>& img, DepthOp op) && noexcept
        -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>;

    // -------------------------------------------------------------------------
    // Semantic Intent Shorthands (Tier 1)
    // -------------------------------------------------------------------------
    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto ClearColor(const TypedImage<Layout, Format>& img, ZHLN::Color4 clearColor) && noexcept {
        return std::move(*this).AddColor(img, ColorOp::Clear(clearColor));
    }

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto LoadColor(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddColor(img, ColorOp::Load());
    }

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto OverwriteColor(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddColor(img, ColorOp::Overwrite());
    }

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto ClearDepth(const TypedImage<Layout, Format>& img, float clearVal = 1.0F) && noexcept {
        return std::move(*this).AddDepth(img, DepthOp::Clear(clearVal));
    }

    template <Vk::ImageLayout Layout, Vk::Format Format>
    constexpr auto LoadDepth(const TypedImage<Layout, Format>& img) && noexcept {
        return std::move(*this).AddDepth(img, DepthOp::Load());
    }

    template <Vk::ImageLayout Layout, Vk::Format Format>
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
    VkFormat                                                      _depthFormat = VK_FORMAT_UNDEFINED;
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
    const char*                   name = "Unnamed Pass";
    std::span<const ImageBarrier> transitions;
    PassRecordFn                  record    = nullptr;
    const void*                   pUserData = nullptr;
};

void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept;

void ClearAttachment(
    VkCommandBuffer    cmd,
    Vk::ImageAspect aspectMask,
    VkExtent2D         extent,
    VkClearValue       value,
    uint32_t           baseArrayLayer = 0,
    uint32_t           layerCount     = 1
) noexcept;

} // namespace ZHLN::Vk

#include "DynamicRendering.inl"
