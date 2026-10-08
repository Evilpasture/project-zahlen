// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

enum class StagingError : uint8_t {
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Host memory allocation failed for staging buffer">{}) = 1,
    OutOfDeviceMemory ZHLN_ANNOTATION(ZHLN::Description<"Device/Host-visible VRAM allocation failed for staging buffer">{}),
    MemoryMappingFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to map staging buffer CPU pointer">{}),
    StagingSpaceExhausted ZHLN_ANNOTATION(ZHLN::Description<"Staging ring buffer has no room for this upload">{}),
    InvalidBufferDimensions ZHLN_ANNOTATION(ZHLN::Description<"Image upload byte size or dimensions exceed limit">{}),
};

class Allocator;
class Buffer;
class StagingContext;

// Owns everything a submitted upload needs until its fence has signaled.
// Destroying this object waits before freeing staging buffers or the pool.
class SubmittedStagingWork {
  public:
    SubmittedStagingWork() = delete;
    ~SubmittedStagingWork() noexcept;
    SubmittedStagingWork(const SubmittedStagingWork&) = delete;
    auto operator=(const SubmittedStagingWork&) -> SubmittedStagingWork& = delete;
    SubmittedStagingWork(SubmittedStagingWork&& other) noexcept;
    auto operator=(SubmittedStagingWork&&) -> SubmittedStagingWork& = delete;

    void Wait() const noexcept;

  private:
    friend class StagingContext;
    SubmittedStagingWork(
        Allocator& allocator, const Context& ctx, CommandPool<QueueType::Graphics>&& pool, std::vector<Buffer>&& buffers, VkFence fence
    ) noexcept;

    Allocator*                       _allocator;
    const Context*                   _ctx;
    CommandPool<QueueType::Graphics> _cmdPool;
    std::vector<Buffer>             _stagingBuffers;
    VkFence                         _fence = VK_NULL_HANDLE;
};

// A single recording batch: Begin constructs its recorder exactly once, and
// ExecuteAsync consumes it into SubmittedStagingWork. There is no reusable
// empty recorder slot or API that can overwrite an active recording.
class StagingContext {
  public:
    StagingContext() = delete;
    ~StagingContext() noexcept;
    StagingContext(const StagingContext&) = delete;
    auto operator=(const StagingContext&) -> StagingContext& = delete;
    StagingContext(StagingContext&& other) noexcept;
    auto operator=(StagingContext&&) -> StagingContext& = delete;

    [[nodiscard]] static auto Begin(Allocator& allocator, const Context& ctx) noexcept -> std::expected<StagingContext, Vk::Error>;

    [[nodiscard]] auto
        UploadImage2D(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, const void* data, size_t bytes) noexcept -> std::expected<void, Vk::Error>;

    void UploadImage2DBuffer(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, VkBuffer stagingBuf, VkDeviceSize offset);
    void UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize, uint32_t mipLevels);
    void AddBuffer(Buffer&& buf);

    [[nodiscard]] auto ExecuteAsync() && -> std::expected<SubmittedStagingWork, Vk::Error>;
    void Abort() && noexcept;

  private:
    StagingContext(Allocator& allocator, const Context& ctx, CommandPool<QueueType::Graphics>&& pool, CommandRecorder&& recorder) noexcept;

    Allocator*                       _allocator;
    const Context*                   _ctx;
    CommandPool<QueueType::Graphics> _cmdPool;
    CommandRecorder                  _recorder;
    std::vector<Buffer>             _stagingBuffers;
};

static_assert(!std::is_default_constructible_v<StagingContext> && !std::is_move_assignable_v<StagingContext>);
static_assert(std::is_nothrow_move_constructible_v<StagingContext>);
static_assert(!std::is_default_constructible_v<SubmittedStagingWork> && !std::is_move_assignable_v<SubmittedStagingWork>);
static_assert(std::is_nothrow_move_constructible_v<SubmittedStagingWork>);

}
