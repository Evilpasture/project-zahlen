// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/ErrorCode.hpp>

namespace ZHLN::Vk {

enum class SamplerFilter : uint8_t {
    Linear,
    Nearest,
};

enum class SamplerAddressMode : uint8_t {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

// Declarative sampler intent. Vulkan create-info construction is confined to
// SamplerConfig.cpp, including descriptor-heap sampler writes.
struct SamplerConfig {
    SamplerFilter      filter = SamplerFilter::Linear;
    SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
    SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
    SamplerAddressMode addressModeW = SamplerAddressMode::Repeat;
    VkBorderColor      borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    float              minLod = 0.0F;
    float              maxLod = VK_LOD_CLAMP_NONE;
    float              maxAnisotropy = 1.0F;
    VkCompareOp        compareOp = VK_COMPARE_OP_ALWAYS;
    bool               anisotropy = false;
    bool               compare = false;

    [[nodiscard]] static constexpr auto LinearRepeat() noexcept -> SamplerConfig {
        return {};
    }
    [[nodiscard]] static constexpr auto NearestRepeat() noexcept -> SamplerConfig {
        return {.filter = SamplerFilter::Nearest};
    }
    [[nodiscard]] static constexpr auto LinearClampToEdge() noexcept -> SamplerConfig {
        return {
            .addressModeU = SamplerAddressMode::ClampToEdge,
            .addressModeV = SamplerAddressMode::ClampToEdge,
            .addressModeW = SamplerAddressMode::ClampToEdge,
        };
    }
    [[nodiscard]] static constexpr auto LinearClampToBorder(VkBorderColor color) noexcept -> SamplerConfig {
        return {
            .addressModeU = SamplerAddressMode::ClampToBorder,
            .addressModeV = SamplerAddressMode::ClampToBorder,
            .addressModeW = SamplerAddressMode::ClampToBorder,
            .borderColor = color,
        };
    }

    [[nodiscard]] constexpr auto WithFilter(SamplerFilter value) const noexcept -> SamplerConfig {
        SamplerConfig result = *this;
        result.filter = value;
        return result;
    }
    [[nodiscard]] constexpr auto WithAddressMode(SamplerAddressMode value) const noexcept -> SamplerConfig {
        return WithAddressModes(value, value, value);
    }
    [[nodiscard]] constexpr auto WithAddressModes(SamplerAddressMode u, SamplerAddressMode v, SamplerAddressMode w) const noexcept -> SamplerConfig {
        SamplerConfig result = *this;
        result.addressModeU = u;
        result.addressModeV = v;
        result.addressModeW = w;
        return result;
    }
    [[nodiscard]] constexpr auto WithBorderColor(VkBorderColor value) const noexcept -> SamplerConfig {
        SamplerConfig result = *this;
        result.addressModeU = SamplerAddressMode::ClampToBorder;
        result.addressModeV = SamplerAddressMode::ClampToBorder;
        result.addressModeW = SamplerAddressMode::ClampToBorder;
        result.borderColor = value;
        return result;
    }
    [[nodiscard]] constexpr auto WithLodRange(float min, float max) const noexcept -> SamplerConfig {
        SamplerConfig result = *this;
        result.minLod = min;
        result.maxLod = max;
        return result;
    }
    [[nodiscard]] constexpr auto WithAnisotropy(float max) const noexcept -> SamplerConfig {
        SamplerConfig result = *this;
        result.anisotropy = true;
        result.maxAnisotropy = max;
        return result;
    }
    [[nodiscard]] constexpr auto WithDepthCompare(VkCompareOp op = VK_COMPARE_OP_LESS_OR_EQUAL) const noexcept -> SamplerConfig {
        SamplerConfig result = *this;
        result.compare = true;
        result.compareOp = op;
        return result;
    }

    [[nodiscard]] auto Create(VkDevice device) const noexcept -> std::expected<Sampler, ErrorCode>;

};

}
