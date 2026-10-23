// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstdint>
#include <type_traits>

namespace ZHLN::Vk {

enum class SamplerFilter : uint8_t {
    Nearest,
    Linear,
};

enum class SamplerAddressMode : uint8_t {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

// A value-type description of a sampler. This is what renderer code passes
// around and what a write batch holds between registration and flush; the
// 80-byte VkSamplerCreateInfo is synthesized on the stack only where the API
// consumes one (AsCreateInfo(), at the vkWriteSamplerDescriptorsEXT call).
//
// Sixteen bytes, packed the way ImageView packs a view and GeometryView packs a
// draw range: every choice that fits in a token -- filter, three address modes,
// border colour, compare op, and the two on/off switches -- lives in one state
// word, and the three numbers that need their full range (min LOD, max LOD,
// anisotropy) sit beside it. A vector of Samplers costs a fifth of a vector of
// create infos, so a batch that describes nine material variants, or nine
// hundred, is a description list rather than a driver-struct array.
//
// Tell, don't ask -- and that includes the read side:
//   * builders return the new value (WithFilter, WithLodRange, ...), so a
//     configuration is one expression with no half-built object in it;
//   * the properties a legal sampler cannot separate are set together --
//     WithBorderColor selects ClampToBorder on every axis, WithAnisotropy
//     raises the filter to Linear (VUID-VkSamplerCreateInfo-anisotropyEnable-
//     01070 / 01071) -- because neither is a choice a caller should have to
//     remember;
//   * the questions it answers are domain questions (Mipmapped(),
//     ClampsToBorder(), Anisotropic(), DepthCompare(), Valid()), never a field
//     read back. There is no MinLod() or Filter(): nothing outside needs to
//     dismantle a sampler and re-make a decision on its behalf, and the packing
//     stays free to change (the LOD window could become half-floats) without
//     touching a call site;
//   * AsCreateInfo() is the only place the Vulkan spelling exists: no call site
//     assembles sType, pNext, mipLodBias or unnormalizedCoordinates by hand.
class Sampler {
  public:
    constexpr Sampler() noexcept = default;

    // -------------------------------------------------------------------------
    // Static Factories
    // -------------------------------------------------------------------------

    [[nodiscard]] static constexpr auto LinearRepeat() noexcept -> Sampler {
        return {};
    }

    [[nodiscard]] static constexpr auto NearestRepeat() noexcept -> Sampler {
        return Sampler {}.WithFilter(SamplerFilter::Nearest);
    }

    [[nodiscard]] static constexpr auto LinearClampToEdge() noexcept -> Sampler {
        return Sampler {}.WithAddressMode(SamplerAddressMode::ClampToEdge);
    }

    [[nodiscard]] static constexpr auto NearestClampToEdge() noexcept -> Sampler {
        return NearestRepeat().WithAddressMode(SamplerAddressMode::ClampToEdge);
    }

    [[nodiscard]] static constexpr auto LinearClampToBorder(VkBorderColor color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK) noexcept -> Sampler {
        return Sampler {}.WithBorderColor(color);
    }

    // The depth-comparison sampler a shadow map is read through: linear, clamped
    // to border so a lookup outside the map compares as "far", and the compare
    // enabled. The three settings travel together because a shadow sampler with
    // any one of them missing reads the depth texture as a colour.
    [[nodiscard]] static constexpr auto Shadow(VkCompareOp op = VK_COMPARE_OP_LESS_OR_EQUAL) noexcept -> Sampler {
        return LinearClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).WithDepthCompare(op);
    }

    // -------------------------------------------------------------------------
    // Fluent Configuration ("Tell, Don't Ask")
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr auto WithFilter(SamplerFilter value) const noexcept -> Sampler {
        Sampler result = *this;
        result._state  = Field(result._state, kFilterShift, kTwoBitMask, FilterToken(value));
        return result;
    }

    [[nodiscard]] constexpr auto WithAddressMode(SamplerAddressMode value) const noexcept -> Sampler {
        return WithAddressModes(value, value, value);
    }

    [[nodiscard]] constexpr auto WithAddressModes(SamplerAddressMode u, SamplerAddressMode v, SamplerAddressMode w) const noexcept -> Sampler {
        Sampler result = *this;
        result._state  = Field(
            Field(Field(result._state, kAddressUShift, kTwoBitMask, AddressToken(u)), kAddressVShift, kTwoBitMask, AddressToken(v)),
            kAddressWShift,
            kTwoBitMask,
            AddressToken(w)
        );
        return result;
    }

    // A border colour only exists where the sampler clamps to the border, so the
    // colour selects the mode with it: a sampler that carries a colour cannot be
    // one whose border is never read.
    [[nodiscard]] constexpr auto WithBorderColor(VkBorderColor value) const noexcept -> Sampler {
        Sampler result = WithAddressMode(SamplerAddressMode::ClampToBorder);
        result._state  = Field(result._state, kBorderShift, kThreeBitMask, BorderToken(value));
        return result;
    }

    [[nodiscard]] constexpr auto WithLodRange(float min, float max) const noexcept -> Sampler {
        Sampler result = *this;
        result._minLod = min;
        result._maxLod = max;
        return result;
    }

    // Sample level 0 only: what a lookup-table texel or a compute grid wants
    // when its storage has a single mip and a wider range would only be inert.
    [[nodiscard]] constexpr auto WithoutMips() const noexcept -> Sampler {
        return WithLodRange(0.0F, 0.0F);
    }

    // Anisotropic filtering is linear filtering by definition, so this raises the
    // filter with the anisotropy instead of letting the caller build a create
    // info the driver rejects.
    [[nodiscard]] constexpr auto WithAnisotropy(float max) const noexcept -> Sampler {
        Sampler result        = WithFilter(SamplerFilter::Linear);
        result._maxAnisotropy = max < 1.0F ? 1.0F : max;
        result._state |= kAnisotropyBit;
        return result;
    }

    // Off means off in exactly one way: the bound goes back to 1 with the switch,
    // so two samplers that behave alike are the same value (operator== is the
    // dedup test a batch or a cache would use).
    [[nodiscard]] constexpr auto WithoutAnisotropy() const noexcept -> Sampler {
        Sampler result        = *this;
        result._state         = result._state & ~kAnisotropyBit;
        result._maxAnisotropy = 1.0F;
        return result;
    }

    [[nodiscard]] constexpr auto WithDepthCompare(VkCompareOp op = VK_COMPARE_OP_LESS_OR_EQUAL) const noexcept -> Sampler {
        Sampler result = *this;
        result._state  = Field(result._state, kCompareOpShift, kThreeBitMask, CompareOpToken(op)) | kCompareBit;
        return result;
    }

    [[nodiscard]] constexpr auto WithoutDepthCompare() const noexcept -> Sampler {
        Sampler result = *this;
        result._state  = Field(result._state & ~kCompareBit, kCompareOpShift, kThreeBitMask, CompareOpToken(VK_COMPARE_OP_ALWAYS));
        return result;
    }

    // -------------------------------------------------------------------------
    // Semantic Predicates ("Ask what it does, not what it holds")
    // -------------------------------------------------------------------------

    // A LOD window with height filters across mips; a flat window reads one level.
    [[nodiscard]] constexpr auto Mipmapped() const noexcept -> bool {
        return _maxLod > _minLod;
    }

    // The border colour is only ever read on an axis that clamps to it.
    // Defined after the class: the lane comparison names the token encoder, and a
    // constant-evaluated body can only call a member whose definition the class
    // has already reached.
    [[nodiscard]] constexpr auto ClampsToBorder() const noexcept -> bool;

    [[nodiscard]] constexpr auto Anisotropic() const noexcept -> bool {
        return (_state & kAnisotropyBit) != 0;
    }

    [[nodiscard]] constexpr auto DepthCompare() const noexcept -> bool {
        return (_state & kCompareBit) != 0;
    }

    // The LOD window and the anisotropy bound are the only ways to build a
    // create info the driver rejects: a negative or inverted window, or an
    // anisotropy below 1. ImageView answers the same question the same way.
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _minLod >= 0.0F && _maxLod >= _minLod && _maxAnisotropy >= 1.0F;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    // -------------------------------------------------------------------------
    // "Tell, Don't Ask": Driver Synthesis
    // -------------------------------------------------------------------------

    /// Synthesizes the 80-byte C struct onto the stack, only where the API takes one.
    [[nodiscard]] constexpr auto AsCreateInfo() const noexcept -> VkSamplerCreateInfo {
        const uint32_t filterToken = TokenAt(kFilterShift, kTwoBitMask);
        const bool     anisotropic = Anisotropic();
        return {
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .magFilter               = DecodeFilter(filterToken),
            .minFilter               = DecodeFilter(filterToken),
            .mipmapMode              = DecodeMipmapMode(filterToken),
            .addressModeU            = DecodeAddress(TokenAt(kAddressUShift, kTwoBitMask)),
            .addressModeV            = DecodeAddress(TokenAt(kAddressVShift, kTwoBitMask)),
            .addressModeW            = DecodeAddress(TokenAt(kAddressWShift, kTwoBitMask)),
            .mipLodBias              = 0.0F,
            .anisotropyEnable        = anisotropic ? VK_TRUE : VK_FALSE,
            .maxAnisotropy           = anisotropic ? _maxAnisotropy : 1.0F,
            .compareEnable           = DepthCompare() ? VK_TRUE : VK_FALSE,
            .compareOp               = DecodeCompareOp(TokenAt(kCompareOpShift, kThreeBitMask)),
            .minLod                  = _minLod,
            .maxLod                  = _maxLod,
            .borderColor             = DecodeBorder(TokenAt(kBorderShift, kThreeBitMask)),
            .unnormalizedCoordinates = VK_FALSE,
        };
    }

    friend constexpr auto operator==(const Sampler&, const Sampler&) noexcept -> bool = default;

  private:
    // The state word: four two-bit lanes and two three-bit lanes hold every
    // choice a sampler makes, so only the float payload is left outside it. The
    // word is kept canonical -- a switch that is off contributes its default, not
    // the last value it held -- so operator== compares behaviour, not history.
    static constexpr uint32_t kFilterShift    = 0;
    static constexpr uint32_t kAddressUShift  = 2;
    static constexpr uint32_t kAddressVShift  = 4;
    static constexpr uint32_t kAddressWShift  = 6;
    static constexpr uint32_t kBorderShift    = 8;
    static constexpr uint32_t kCompareOpShift = 11;
    static constexpr uint32_t kAnisotropyBit  = 1u << 14;
    static constexpr uint32_t kCompareBit     = 1u << 15;
    static constexpr uint32_t kTwoBitMask     = 0x3u;
    static constexpr uint32_t kThreeBitMask   = 0x7u;

    // One lane written, every other bit of the word untouched. The mask is the
    // lane's own width: the two-bit fields and the three-bit ones share this
    // helper, and a width that did not match the shift would splice two lanes.
    [[nodiscard]] static constexpr auto Field(uint32_t state, uint32_t shift, uint32_t mask, uint32_t token) noexcept -> uint32_t {
        return (state & ~(mask << shift)) | (token << shift);
    }

    // The read side of Field(), private for the same reason: only this class
    // looks into the word, and it looks at tokens, never at an enum or a float
    // that a caller could recombine into a decision of its own.
    [[nodiscard]] constexpr auto TokenAt(uint32_t shift, uint32_t mask) const noexcept -> uint32_t {
        return (_state >> shift) & mask;
    }

    // Slang-free vocabulary in, tokens out. A token that does not round-trip is a
    // value this engine does not carry; it is clamped to the default rather than
    // written into the word, where it would decode as some other choice.
    [[nodiscard]] static constexpr auto FilterToken(SamplerFilter value) noexcept -> uint32_t {
        switch (value) {
            case SamplerFilter::Nearest: return 0;
            case SamplerFilter::Linear: return 1;
        }
        return 1;
    }

    [[nodiscard]] static constexpr auto AddressToken(SamplerAddressMode value) noexcept -> uint32_t {
        switch (value) {
            case SamplerAddressMode::Repeat: return 0;
            case SamplerAddressMode::MirroredRepeat: return 1;
            case SamplerAddressMode::ClampToEdge: return 2;
            case SamplerAddressMode::ClampToBorder: return 3;
        }
        return 0;
    }

    [[nodiscard]] static constexpr auto BorderToken(VkBorderColor value) noexcept -> uint32_t {
        switch (value) {
            case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK: return 0;
            case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK: return 1;
            case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK: return 2;
            case VK_BORDER_COLOR_INT_OPAQUE_BLACK: return 3;
            case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE: return 4;
            case VK_BORDER_COLOR_INT_OPAQUE_WHITE: return 5;
            default: return 0;
        }
    }

    [[nodiscard]] static constexpr auto CompareOpToken(VkCompareOp value) noexcept -> uint32_t {
        switch (value) {
            case VK_COMPARE_OP_NEVER: return 0;
            case VK_COMPARE_OP_LESS: return 1;
            case VK_COMPARE_OP_EQUAL: return 2;
            case VK_COMPARE_OP_LESS_OR_EQUAL: return 3;
            case VK_COMPARE_OP_GREATER: return 4;
            case VK_COMPARE_OP_NOT_EQUAL: return 5;
            case VK_COMPARE_OP_GREATER_OR_EQUAL: return 6;
            case VK_COMPARE_OP_ALWAYS: return 7;
            default: return 7;
        }
    }

    // Tokens out to the driver spelling. Each compares against the token its
    // encoder produces, so there is one definition of every ordering, and a
    // token that is not recognised decodes as the default AsCreateInfo() would
    // have written had the caller never touched the field.
    [[nodiscard]] static constexpr auto DecodeFilter(uint32_t token) noexcept -> VkFilter {
        return token == FilterToken(SamplerFilter::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }
    [[nodiscard]] static constexpr auto DecodeMipmapMode(uint32_t token) noexcept -> VkSamplerMipmapMode {
        return token == FilterToken(SamplerFilter::Nearest) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
    [[nodiscard]] static constexpr auto DecodeAddress(uint32_t token) noexcept -> VkSamplerAddressMode {
        switch (token) {
            case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    [[nodiscard]] static constexpr auto DecodeBorder(uint32_t token) noexcept -> VkBorderColor {
        switch (token) {
            case 1: return VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
            case 2: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            case 3: return VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            case 4: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            case 5: return VK_BORDER_COLOR_INT_OPAQUE_WHITE;
            default: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        }
    }
    [[nodiscard]] static constexpr auto DecodeCompareOp(uint32_t token) noexcept -> VkCompareOp {
        switch (token) {
            case 0: return VK_COMPARE_OP_NEVER;
            case 1: return VK_COMPARE_OP_LESS;
            case 2: return VK_COMPARE_OP_EQUAL;
            case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case 4: return VK_COMPARE_OP_GREATER;
            case 5: return VK_COMPARE_OP_NOT_EQUAL;
            case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            default: return VK_COMPARE_OP_ALWAYS;
        }
    }

    // The default is LinearRepeat: filter = Linear, address modes = Repeat, border
    // = float transparent black, compare op = Always (inert while the compare is
    // off), anisotropy off and bound to 1.
    static constexpr auto DefaultState() noexcept -> uint32_t {
        return (FilterToken(SamplerFilter::Linear) << kFilterShift) | (CompareOpToken(VK_COMPARE_OP_ALWAYS) << kCompareOpShift);
    }

    uint32_t _state         = DefaultState();    // 4 bytes: see the token lanes above
    float    _minLod        = 0.0F;              // 4 bytes
    float    _maxLod        = VK_LOD_CLAMP_NONE; // 4 bytes
    float    _maxAnisotropy = 1.0F;              // 4 bytes
};

[[nodiscard]] constexpr auto Sampler::ClampsToBorder() const noexcept -> bool {
    constexpr uint32_t border = AddressToken(SamplerAddressMode::ClampToBorder);
    return TokenAt(kAddressUShift, kTwoBitMask) == border || TokenAt(kAddressVShift, kTwoBitMask) == border
           || TokenAt(kAddressWShift, kTwoBitMask) == border;
}

static_assert(sizeof(Sampler) == 16, "Sampler must be exactly 16 bytes: a batch is a list of these, not of create infos.");
static_assert(std::is_trivially_copyable_v<Sampler>);

// The checks below assert on AsCreateInfo(): the driver contract is what matters,
// not the private word that happens to hold it. Nothing here -- and nothing in
// the engine -- reads a field back.

// The default is the linear-repeat sampler, and every Vulkan-only field is fixed.
static_assert(Sampler {}.AsCreateInfo().magFilter == VK_FILTER_LINEAR && Sampler {}.AsCreateInfo().minFilter == VK_FILTER_LINEAR);
static_assert(Sampler {}.AsCreateInfo().mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
static_assert(Sampler {}.AsCreateInfo().addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT);
static_assert(Sampler {}.AsCreateInfo().addressModeV == VK_SAMPLER_ADDRESS_MODE_REPEAT);
static_assert(Sampler {}.AsCreateInfo().addressModeW == VK_SAMPLER_ADDRESS_MODE_REPEAT);
static_assert(Sampler {}.AsCreateInfo().anisotropyEnable == VK_FALSE && Sampler {}.AsCreateInfo().maxAnisotropy == 1.0F);
static_assert(Sampler {}.AsCreateInfo().compareEnable == VK_FALSE && Sampler {}.AsCreateInfo().compareOp == VK_COMPARE_OP_ALWAYS);
static_assert(Sampler {}.AsCreateInfo().borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
static_assert(Sampler {}.AsCreateInfo().maxLod == VK_LOD_CLAMP_NONE && Sampler {}.AsCreateInfo().unnormalizedCoordinates == VK_FALSE);
static_assert(Sampler {}.Valid() && Sampler {}.Mipmapped()); // the default LOD window spans 0..clamp-none

// Presets reach the driver as the filter they name.
static_assert(Sampler::NearestClampToEdge().AsCreateInfo().minFilter == VK_FILTER_NEAREST);
static_assert(Sampler::NearestClampToEdge().AsCreateInfo().mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
static_assert(Sampler::NearestClampToEdge().AsCreateInfo().addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
static_assert(Sampler::NearestClampToEdge().AsCreateInfo().addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

// Each address mode lands in its own lane: a mask that was too wide would splice
// U, V and W together, which the driver would report as three equal axes.
static_assert([] {
    const VkSamplerCreateInfo info =
        Sampler::LinearRepeat().WithAddressModes(SamplerAddressMode::Repeat, SamplerAddressMode::ClampToEdge, SamplerAddressMode::MirroredRepeat).AsCreateInfo();
    return info.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT && info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
           && info.addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
}());

// A border colour selects the clamp-to-border mode with it, on every axis.
static_assert(Sampler::LinearClampToBorder().AsCreateInfo().borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
static_assert(Sampler::LinearClampToBorder().AsCreateInfo().addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
static_assert(Sampler::LinearClampToBorder(VK_BORDER_COLOR_INT_OPAQUE_WHITE).AsCreateInfo().borderColor == VK_BORDER_COLOR_INT_OPAQUE_WHITE);

// The shadow sampler is compare + border + linear in one value.
static_assert(Sampler::Shadow().AsCreateInfo().compareEnable == VK_TRUE);
static_assert(Sampler::Shadow().AsCreateInfo().compareOp == VK_COMPARE_OP_LESS_OR_EQUAL);
static_assert(Sampler::Shadow().AsCreateInfo().borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
static_assert(Sampler::Shadow(VK_COMPARE_OP_GREATER).AsCreateInfo().compareOp == VK_COMPARE_OP_GREATER);
static_assert(Sampler::Shadow().DepthCompare() && Sampler::Shadow().ClampsToBorder());

// Anisotropy promotes the filter, and a bound below 1 is clamped rather than
// written into the create info.
static_assert(Sampler::NearestRepeat().WithAnisotropy(8.0F).AsCreateInfo().minFilter == VK_FILTER_LINEAR);
static_assert(Sampler::NearestRepeat().WithAnisotropy(8.0F).AsCreateInfo().anisotropyEnable == VK_TRUE);
static_assert(Sampler::NearestRepeat().WithAnisotropy(8.0F).AsCreateInfo().maxAnisotropy == 8.0F);
static_assert(Sampler::NearestRepeat().WithAnisotropy(0.25F).AsCreateInfo().maxAnisotropy == 1.0F);
static_assert(!Sampler::NearestRepeat().WithAnisotropy(8.0F).WithoutAnisotropy().AsCreateInfo().anisotropyEnable);

// A flat LOD window reads one level; a raised one filters across mips.
static_assert(Sampler::LinearRepeat().WithoutMips().AsCreateInfo().maxLod == 0.0F);
static_assert(!Sampler::LinearRepeat().WithoutMips().Mipmapped());
static_assert(Sampler::LinearRepeat().WithLodRange(0.0F, 5.0F).Mipmapped());

// Only a border clamp makes the border colour reachable.
static_assert(Sampler::LinearClampToBorder().ClampsToBorder() && !Sampler::LinearRepeat().ClampsToBorder());
static_assert(!Sampler::LinearRepeat().WithAddressModes(SamplerAddressMode::Repeat, SamplerAddressMode::ClampToEdge, SamplerAddressMode::Repeat).ClampsToBorder());

// Validity is the pair of float ranges, and nothing else.
static_assert(Sampler {}.Valid() && static_cast<bool>(Sampler {}));
static_assert(!Sampler::LinearRepeat().WithLodRange(4.0F, 1.0F).Valid());
static_assert(!Sampler::LinearRepeat().WithLodRange(-1.0F, 1.0F).Valid());

// Turning something off restores the default, so behaviour -- not history -- is
// what operator== compares: both round trips land back on the plain sampler.
static_assert(Sampler::LinearRepeat().WithDepthCompare().WithoutDepthCompare() == Sampler::LinearRepeat());
static_assert(Sampler::LinearRepeat().WithAnisotropy(8.0F).WithoutAnisotropy() == Sampler::LinearRepeat());
static_assert(Sampler::NearestRepeat() != Sampler::LinearRepeat());
static_assert(Sampler::Shadow() == Sampler::LinearClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).WithDepthCompare());

} // namespace ZHLN::Vk
