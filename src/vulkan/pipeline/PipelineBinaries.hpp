// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <concepts>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

class PipelineBinaryStore {
  public:
    void Init(VkDevice device, bool enabled, std::span<const uint8_t> initialData) noexcept;

    [[nodiscard]] bool Enabled() const noexcept {
        return _enabled;
    }

    [[nodiscard]] auto Find(std::span<const uint8_t> key) const noexcept -> std::optional<std::vector<uint8_t>>;
    void               Insert(std::span<const uint8_t> key, std::span<const uint8_t> data) noexcept;
    void               Erase(std::span<const uint8_t> key) noexcept;

    [[nodiscard]] auto Serialize() const noexcept -> std::vector<uint8_t>;

  private:
    struct Entry {
        std::vector<uint8_t> key;
        std::vector<uint8_t> data;
    };

    bool                 _enabled = false;
    std::vector<uint8_t> _globalKey;
    std::vector<Entry>   _entries;
    mutable std::mutex   _mutex;
};

class PipelineBinarySession {
  public:
    PipelineBinarySession() = default;

    PipelineBinarySession(const PipelineBinarySession&)                        = delete;
    auto operator=(const PipelineBinarySession&) -> PipelineBinarySession&     = delete;
    PipelineBinarySession(PipelineBinarySession&&) noexcept                    = default;
    auto operator=(PipelineBinarySession&&) noexcept -> PipelineBinarySession& = default;

    [[nodiscard]] static auto Begin(VkDevice device, PipelineBinaryStore* store, const VkPipelineCreateInfoKHR& createInfo) noexcept -> PipelineBinarySession;

    template <typename CreateInfo>
        requires std::same_as<std::remove_cvref_t<CreateInfo>, VkGraphicsPipelineCreateInfo> ||
                 std::same_as<std::remove_cvref_t<CreateInfo>, VkComputePipelineCreateInfo>
    [[nodiscard]] static auto Begin(VkDevice device, PipelineBinaryStore* store, const CreateInfo& createInfo) noexcept -> PipelineBinarySession {
        const VkPipelineCreateInfoKHR wrapper {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR,
            .pNext = const_cast<std::remove_cvref_t<CreateInfo>*>(&createInfo),
        };
        return Begin(device, store, wrapper);
    }

    [[nodiscard]] auto Binaries() const noexcept -> std::span<const VkPipelineBinaryKHR> {
        return _raw;
    }

    [[nodiscard]] bool Replayed() const noexcept {
        return !_binaries.empty() && !_capture;
    }

    void Commit() noexcept;

    void Invalidate() noexcept;

  private:
    VkDevice                         _device = VK_NULL_HANDLE;
    PipelineBinaryStore*             _store  = nullptr;
    std::vector<uint8_t>             _key;
    std::vector<PipelineBinary>      _binaries;
    std::vector<VkPipelineBinaryKHR> _raw;
    bool                             _capture = false;
};

} // namespace ZHLN::Vk
