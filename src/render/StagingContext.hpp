// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/StagingContext.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

class Allocator;
class Buffer;

class StagingContext {
  public:
    StagingContext(Allocator& allocator, const Context& ctx);
    ~StagingContext();

    // Disable copying to enforce strict ownership
    StagingContext(const StagingContext&)            = delete;
    StagingContext& operator=(const StagingContext&) = delete;

    StagingContext(StagingContext&& other) noexcept;
    StagingContext& operator=(StagingContext&&) noexcept = delete;

    void Begin();

    [[nodiscard]] auto
        UploadImage2D(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, const void* data, size_t bytes) noexcept -> std::expected<void, VkResult>;

    void UploadImage2DBuffer(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, VkBuffer stagingBuf, VkDeviceSize offset);

    void UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize, uint32_t mipLevels);

    void AddBuffer(Buffer&& buf);

    void ExecuteAsync();

    void Wait() noexcept;

  private:
    Allocator*                       _allocator = nullptr;
    const Context*                   _ctx       = nullptr;
    CommandPool<QueueType::Graphics> _cmdPool;
    VkCommandBuffer                  _cmd = VK_NULL_HANDLE;
    std::vector<Buffer>              _stagingBuffers;
    VkFence                          _fence = VK_NULL_HANDLE; // Owned internally
};

} // namespace ZHLN::Vk
