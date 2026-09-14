// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/PipelineCacheHelper.cpp
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "PipelineCacheHelper.hpp"
#include <Zahlen/Log.hpp>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ZHLN::Vk {

namespace {

/// A driver pipeline cache for this engine is a few megabytes. Anything past
/// this is a corrupt or hostile file, and refusing it keeps the allocation
/// below bounded.
constexpr uint64_t kMaxCacheBytes = 256ull * 1024 * 1024;

/// Reads `path` into `out`. Returns false if the file is missing, empty,
/// oversized, or short-read; `out` is left cleared on failure.
[[nodiscard]] auto ReadWholeFile(std::string_view path, std::vector<uint8_t>& out) -> bool {
    std::error_code           ec;
    const std::filesystem::path file(path);

    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size == 0 || size > kMaxCacheBytes) {
        return false;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }

    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        out.clear();
        return false;
    }
    return true;
}

/// A cache blob is bound to the driver build and GPU that wrote it. Feeding a
/// foreign one to vkCreatePipelineCache is at best ignored and at worst
/// rejected, so compare the header against the live device first.
[[nodiscard]] auto MatchesDevice(const VkPipelineCacheHeaderVersionOne& header, const VkPhysicalDeviceProperties& props) noexcept -> bool {
    return header.headerSize == sizeof(VkPipelineCacheHeaderVersionOne) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
           && header.vendorID == props.vendorID && header.deviceID == props.deviceID
           && std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

} // namespace

auto LoadPipelineCache(const VkDevice device, const VkPhysicalDeviceProperties& props, const std::string_view path) noexcept -> PipelineCache {
    std::vector<uint8_t> data;

    // noexcept contract: a corrupt cache file must never take the process down
    // during init, so the only throwing operation (the allocation) is contained
    // here and simply degrades to an empty cache.
    try {
        if (ReadWholeFile(path, data)) {
            VkPipelineCacheHeaderVersionOne header {};
            if (data.size() < sizeof(header)) {
                ZHLN::Log("[PipelineCache] '{}' is smaller than a cache header; starting empty.", path);
                data.clear();
            } else {
                std::memcpy(&header, data.data(), sizeof(header));
                if (MatchesDevice(header, props)) {
                    ZHLN::Log("[PipelineCache] Reused {} KB from '{}'.", data.size() / 1024, path);
                } else {
                    ZHLN::Log("[PipelineCache] '{}' was written by a different driver or GPU; discarding.", path);
                    data.clear();
                }
            }
        }
    } catch (...) {
        data.clear();
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

void SavePipelineCache(const VkDevice device, const VkPipelineCache cache, const std::string_view path) noexcept {
    if (device == VK_NULL_HANDLE || cache == VK_NULL_HANDLE) {
        return;
    }

    try {
        size_t size = 0;
        if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS || size == 0) {
            return;
        }

        std::vector<uint8_t> data(size);
        if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS) {
            ZHLN::Log("[PipelineCache] vkGetPipelineCacheData failed; nothing written.");
            return;
        }
        // The driver may report a smaller blob on the second call.
        data.resize(size);

        const std::filesystem::path target(path);
        std::error_code             ec;
        if (const auto parent = target.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        // Write to a sibling temp file and rename over the target: a crash or
        // power loss mid-write then leaves the previous good cache in place
        // rather than a truncated one that fails header validation forever.
        const std::filesystem::path temp = std::filesystem::path(path).concat(".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                ZHLN::Log("[PipelineCache] Could not open '{}' for writing.", temp.string());
                return;
            }
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        std::filesystem::rename(temp, target, ec);
        if (ec) {
            ZHLN::Log("[PipelineCache] Renaming '{}' into place failed: {}", temp.string(), ec.message());
            std::filesystem::remove(temp, ec);
            return;
        }
        ZHLN::Log("[PipelineCache] Saved {} KB to '{}'.", data.size() / 1024, target.string());
    } catch (...) {
        ZHLN::Log("[PipelineCache] Save failed; the cache was not written.");
    }
}

} // namespace ZHLN::Vk
