// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "CommandRecorder.hpp"
#include <Zahlen/Error.hpp>
#include <expected>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

// What a stream has to be able to do before it records its first command: put
// the heap bindings and the push-address rows its shaders run against in place
// on the command buffer. It is a callback instead of a type because this layer
// does not know what a frame is or where the heaps live -- whoever owns them
// supplies the step, and the only thing asked of it here is that it cannot fail.
template <typename Fn>
concept StreamStateEstablisher = requires(const Fn& establish, VkCommandBuffer cmd) {
    { establish(cmd) } noexcept;
};

/// A command stream that began life with its heap state established.
///
/// CommandRecorder::Begin records into any command buffer. This records into one
/// whose descriptor heaps are bound and whose frame address block is pushed, and
/// there is no way to obtain a StreamCommandRecorder without supplying the step
/// that does it -- that is the entire reason the type exists. On a heap-based
/// pipeline the failure it removes is not a slower frame: the shaders read
/// `scene.frame`, `scene.lights`, `scene.g_instances` and `scene.g_joints` as
/// raw 64-bit addresses out of the push range, with no descriptor table behind
/// them, so a stream that begins without the block dispatches at address zero
/// (device lost) or at the previous stream's addresses (a frame that tears).
///
/// There are two such streams in a frame -- a window's graphics stream and the
/// async compute stream -- and both begin through RenderFrameContext::BeginStream
/// rather than here, because only the frame context owns the heaps.
///
/// Secondary command buffers are not begun this way: they inherit neither the
/// bindings nor the push range from the primary they are executed in, so
/// ParallelCommandRecorder and ParallelDrawDispatch are handed the same state as
/// inheritance info and push the rows into every slot they record themselves.
class StreamCommandRecorder {
  public:
    /// Begin `cmd` and establish its heap state before anything is recorded into
    /// it. If the buffer cannot be begun, the establisher is never called, so a
    /// stream either exists with its heap state or does not exist at all.
    template <StreamStateEstablisher Establish>
    [[nodiscard]] static auto
        Begin(VkCommandBuffer cmd, Establish&& establish, VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) noexcept
        -> std::expected<StreamCommandRecorder, ErrorCode> {
        auto recording = CommandRecorder::Begin(cmd, flags);
        if (!recording) [[unlikely]] {
            return std::unexpected(recording.error());
        }
        std::forward<Establish>(establish)(cmd);
        return StreamCommandRecorder(std::move(*recording));
    }

    [[nodiscard]] auto Handle() const noexcept -> VkCommandBuffer {
        return _recorder.Handle();
    }
    [[nodiscard]] auto IsRecording() const noexcept -> bool {
        return _recorder.IsRecording();
    }
    explicit operator bool() const noexcept {
        return IsRecording();
    }

    /// Hand the recording to its owner. The stream's heap state is already on
    /// the command buffer, so nothing downstream has to carry this type around
    /// to keep it: FrameDestinations stores the plain recorder and every later
    /// reader only needs the handle.
    [[nodiscard]] auto Release() && noexcept -> CommandRecorder {
        return std::move(_recorder);
    }

    [[nodiscard]] auto End() && noexcept -> std::expected<ExecutableCommands, ErrorCode> {
        return std::move(_recorder).End();
    }
    void Abort() && noexcept {
        std::move(_recorder).Abort();
    }

  private:
    explicit StreamCommandRecorder(CommandRecorder&& recorder) noexcept: _recorder(std::move(recorder)) {
    }

    CommandRecorder _recorder;
};

static_assert(!std::is_default_constructible_v<StreamCommandRecorder>);
static_assert(!std::is_copy_constructible_v<StreamCommandRecorder> && !std::is_copy_assignable_v<StreamCommandRecorder>);
static_assert(std::is_nothrow_move_constructible_v<StreamCommandRecorder> && !std::is_move_assignable_v<StreamCommandRecorder>);
static_assert(std::is_nothrow_destructible_v<StreamCommandRecorder>);
static_assert(!std::is_constructible_v<StreamCommandRecorder, VkCommandBuffer>);

} // namespace ZHLN::Vk
