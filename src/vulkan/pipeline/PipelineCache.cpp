// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "PipelineCache.hpp"
#include <Zahlen/Log.hpp>
#include <cstring>
#include <vector>

namespace ZHLN::Vk {

namespace {

[[nodiscard]] auto MatchesDevice(const VkPipelineCacheHeaderVersionOne& header, const VkPhysicalDeviceProperties& props) noexcept -> bool {
    return header.headerSize == sizeof(VkPipelineCacheHeaderVersionOne) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
           && header.vendorID == props.vendorID && header.deviceID == props.deviceID
           && std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

} // namespace

auto LoadPipelineCache(const VkDevice device, const VkPhysicalDeviceProperties& props, const std::span<const uint8_t> initialData) noexcept
    -> PipelineCache {
    std::vector<uint8_t> data;

    if (!initialData.empty()) {
        VkPipelineCacheHeaderVersionOne header {};
        if (initialData.size() < sizeof(header)) {
            ZHLN::Log("[PipelineCache] Provided initial data is smaller than a cache header; starting empty.");
        } else {
            std::memcpy(&header, initialData.data(), sizeof(header));
            if (MatchesDevice(header, props)) {
                ZHLN::Log("[PipelineCache] Reused {} KB from provided cache data.", initialData.size() / 1024);
                data.assign(initialData.begin(), initialData.end());
            } else {
                ZHLN::Log("[PipelineCache] Provided cache data was written by a different driver or GPU; discarding.");
            }
        }
    }

    const VkPipelineCacheCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .pNext           = nullptr,
        .flags           = 0,
        .initialDataSize = data.size(),
        .pInitialData    = data.empty() ? nullptr : data.data(),
    };

    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &ci, nullptr, &cache) != VK_SUCCESS) {
        ZHLN::Log("[PipelineCache] vkCreatePipelineCache failed; pipelines will be compiled uncached.");
        return {};
    }
    return PipelineCache(device, cache);
}

auto SavePipelineCache(const VkDevice device, const VkPipelineCache cache) noexcept -> std::vector<uint8_t> {
    if (device == VK_NULL_HANDLE || cache == VK_NULL_HANDLE) {
        return {};
    }

    size_t size = 0;
    if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return {};
    }

    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS) {
        ZHLN::Log("[PipelineCache] vkGetPipelineCacheData failed; nothing extracted.");
        return {};
    }
    data.resize(size);
    return data;
}

} // namespace ZHLN::Vk
