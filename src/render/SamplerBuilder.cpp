// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/SamplerBuilder.cpp
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "SamplerBuilder.hpp"

namespace ZHLN::Vk {

SamplerBuilder::SamplerBuilder() noexcept {
    _info = {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = 0,
        .magFilter               = VK_FILTER_LINEAR,
        .minFilter               = VK_FILTER_LINEAR,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias              = 0.0F,
        .anisotropyEnable        = VK_FALSE,
        .maxAnisotropy           = 1.0F,
        .compareEnable           = VK_FALSE,
        .compareOp               = VK_COMPARE_OP_ALWAYS,
        .minLod                  = 0.0F,
        .maxLod                  = VK_LOD_CLAMP_NONE,
        .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
}

auto SamplerBuilder::Linear() noexcept -> SamplerBuilder& {
    _info.magFilter  = VK_FILTER_LINEAR;
    _info.minFilter  = VK_FILTER_LINEAR;
    _info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    return *this;
}

auto SamplerBuilder::Nearest() noexcept -> SamplerBuilder& {
    _info.magFilter  = VK_FILTER_NEAREST;
    _info.minFilter  = VK_FILTER_NEAREST;
    _info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return *this;
}

auto SamplerBuilder::Repeat() noexcept -> SamplerBuilder& {
    _info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    _info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    _info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return *this;
}

auto SamplerBuilder::ClampToEdge() noexcept -> SamplerBuilder& {
    _info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    _info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    _info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return *this;
}

auto SamplerBuilder::ClampToBorder(VkBorderColor color) noexcept -> SamplerBuilder& {
    _info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    _info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    _info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    _info.borderColor  = color;
    return *this;
}

auto SamplerBuilder::Anisotropy(float maxAniso) noexcept -> SamplerBuilder& {
    _info.anisotropyEnable = VK_TRUE;
    _info.maxAnisotropy    = maxAniso;
    return *this;
}

auto SamplerBuilder::DepthCompare(VkCompareOp op) noexcept -> SamplerBuilder& {
    _info.compareEnable = VK_TRUE;
    _info.compareOp     = op;
    return *this;
}

auto SamplerBuilder::LodRange(float minLod, float maxLod) noexcept -> SamplerBuilder& {
    _info.minLod = minLod;
    _info.maxLod = maxLod;
    return *this;
}

auto SamplerBuilder::Build(VkDevice device) const noexcept -> std::expected<Sampler, ZHLN::Error> {
    if (device == VK_NULL_HANDLE) {
        return std::unexpected(SamplerCreationError::NullDevice);
    }

    VkSampler sampler = nullptr;
    if (vkCreateSampler(device, &_info, nullptr, &sampler) != VK_SUCCESS) {
        return std::unexpected(SamplerCreationError::CreationFailed);
    }

    return Sampler {device, sampler};
}

} // namespace ZHLN::Vk
