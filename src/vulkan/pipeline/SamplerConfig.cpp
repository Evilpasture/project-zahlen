// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "pipeline/DescriptorHeap.hpp"
#include "pipeline/SamplerConfig.hpp"

#include <cmath>

namespace ZHLN::Vk {

namespace {

enum class SamplerCreationError : uint8_t {
    NullDevice ZHLN_ANNOTATION(ZHLN::Description<"Null device for sampler creation">{}) = 1,
    InvalidConfiguration ZHLN_ANNOTATION(ZHLN::Description<"Invalid sampler configuration">{}),
    CreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Sampler creation failed">{}),
};

[[nodiscard]] constexpr auto ToVkFilter(SamplerFilter filter) noexcept -> VkFilter {
    switch (filter) {
        case SamplerFilter::Linear: return VK_FILTER_LINEAR;
        case SamplerFilter::Nearest: return VK_FILTER_NEAREST;
    }
    return VK_FILTER_LINEAR;
}

[[nodiscard]] constexpr auto ToVkMipmapMode(SamplerFilter filter) noexcept -> VkSamplerMipmapMode {
    switch (filter) {
        case SamplerFilter::Linear: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        case SamplerFilter::Nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    return VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

[[nodiscard]] constexpr auto ToVkAddressMode(SamplerAddressMode mode) noexcept -> VkSamplerAddressMode {
    switch (mode) {
        case SamplerAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SamplerAddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SamplerAddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

[[nodiscard]] auto ToVkCreateInfo(const SamplerConfig& config) noexcept -> VkSamplerCreateInfo {
    const VkFilter filterValue = ToVkFilter(config.filter);
    const VkSamplerAddressMode addressU = ToVkAddressMode(config.addressModeU);
    const VkSamplerAddressMode addressV = ToVkAddressMode(config.addressModeV);
    const VkSamplerAddressMode addressW = ToVkAddressMode(config.addressModeW);
    return {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = filterValue,
        .minFilter = filterValue,
        .mipmapMode = ToVkMipmapMode(config.filter),
        .addressModeU = addressU,
        .addressModeV = addressV,
        .addressModeW = addressW,
        .mipLodBias = 0.0F,
        .anisotropyEnable = config.anisotropy ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = config.anisotropy ? config.maxAnisotropy : 1.0F,
        .compareEnable = config.compare ? VK_TRUE : VK_FALSE,
        .compareOp = config.compareOp,
        .minLod = config.minLod,
        .maxLod = config.maxLod,
        .borderColor = config.borderColor,
        .unnormalizedCoordinates = VK_FALSE,
    };
}

} // namespace

auto SamplerConfig::Create(VkDevice device) const noexcept -> std::expected<Sampler, Vk::Error> {
    using enum SamplerCreationError;
    if (device == VK_NULL_HANDLE) {
        return std::unexpected(NullDevice);
    }
    if (!std::isfinite(minLod) || !std::isfinite(maxLod) || minLod > maxLod ||
        (anisotropy && (!std::isfinite(maxAnisotropy) || maxAnisotropy < 1.0F))) {
        return std::unexpected(InvalidConfiguration);
    }

    const VkSamplerCreateInfo info = ToVkCreateInfo(*this);
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return std::unexpected(CreationFailed);
    }
    return Sampler {device, sampler};
}

struct SamplerWriteBatch::Impl {
    std::vector<SamplerConfig> configs;
    std::vector<uint32_t>      slots;
};

SamplerWriteBatch::SamplerWriteBatch() noexcept: _impl(std::make_unique<Impl>()) {
}
SamplerWriteBatch::~SamplerWriteBatch() noexcept = default;

SamplerWriteBatch::SamplerWriteBatch(SamplerWriteBatch&& other) noexcept = default;
auto SamplerWriteBatch::operator=(SamplerWriteBatch&& other) noexcept -> SamplerWriteBatch& = default;

auto SamplerWriteBatch::Empty() const noexcept -> bool {
    return _impl->slots.empty();
}

auto SamplerWriteBatch::SlotCount() const noexcept -> uint32_t {
    return static_cast<uint32_t>(_impl->slots.size());
}

auto SamplerWriteBatch::SlotsData() const noexcept -> const uint32_t* {
    return _impl->slots.data();
}

void SamplerWriteBatch::AddSampler(SamplerHandle handle, const SamplerConfig& config) noexcept {
    if (!handle.Valid()) {
        return;
    }
    _impl->configs.push_back(config);
    _impl->slots.push_back(handle.index);
}

void SamplerWriteBatch::Flush(VkDevice device, void* mappedPtr, VkDeviceSize stride) noexcept {
    const auto totalCount = static_cast<uint32_t>(_impl->slots.size());
    if (totalCount == 0) {
        return;
    }

    std::vector<VkSamplerCreateInfo> createInfos;
    createInfos.reserve(totalCount);
    for (const SamplerConfig& config: _impl->configs) {
        createInfos.push_back(ToVkCreateInfo(config));
    }

    std::vector<VkHostAddressRangeEXT> ranges(totalCount);
    for (uint32_t i = 0; i < totalCount; ++i) {
        ranges[i] = {.address = static_cast<uint8_t*>(mappedPtr) + (_impl->slots[i] * stride), .size = stride};
    }

    vkWriteSamplerDescriptorsEXT(device, totalCount, createInfos.data(), ranges.data());
    _impl->configs.clear();
    _impl->slots.clear();
}

}
