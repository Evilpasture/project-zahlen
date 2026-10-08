// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

enum class CommandRecordingError : uint8_t {
    NullCommandBuffer ZHLN_ANNOTATION(ZHLN::Description<"Cannot begin a null command buffer">{}) = 1,
    NotRecording ZHLN_ANNOTATION(ZHLN::Description<"Command buffer is not recording">{}),
    NotExecutable ZHLN_ANNOTATION(ZHLN::Description<"Command buffer has not been ended">{}),
};

// A non-owning proof that vkEndCommandBuffer succeeded. The command pool still
// owns the buffer and must outlive this token and any GPU work submitted with it.
class ExecutableCommands {
  public:
    ExecutableCommands() = delete;
    ~ExecutableCommands() noexcept = default;
    ExecutableCommands(const ExecutableCommands&) = delete;
    auto operator=(const ExecutableCommands&) -> ExecutableCommands& = delete;
    ExecutableCommands(ExecutableCommands&& other) noexcept: _cmd(std::exchange(other._cmd, VK_NULL_HANDLE)) {}
    auto operator=(ExecutableCommands&&) -> ExecutableCommands& = delete;

    [[nodiscard]] auto Handle() const noexcept -> VkCommandBuffer { return _cmd; }
    [[nodiscard]] auto Valid() const noexcept -> bool { return _cmd != VK_NULL_HANDLE; }
    explicit operator bool() const noexcept { return Valid(); }

  private:
    friend class CommandRecorder;
    explicit ExecutableCommands(VkCommandBuffer cmd) noexcept: _cmd(cmd) {}
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
};

// The Recording -> Executable transition is explicit; dropping a recorder does
// not end or reset a Vulkan command buffer. Abort() resets it to Initial and
// requires a pool created with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
// (all CommandPool instances in this renderer have that flag).
class CommandRecorder {
  public:
    CommandRecorder() = delete;
    ~CommandRecorder() noexcept = default;
    CommandRecorder(const CommandRecorder&) = delete;
    auto operator=(const CommandRecorder&) -> CommandRecorder& = delete;
    CommandRecorder(CommandRecorder&& other) noexcept: _cmd(std::exchange(other._cmd, VK_NULL_HANDLE)) {}
    // A recording can be transferred to a new owner, never replaced in place.
    // There is no "only when empty" precondition hidden in a move assignment.
    auto operator=(CommandRecorder&&) -> CommandRecorder& = delete;

    [[nodiscard]] static auto Begin(
        VkCommandBuffer cmd,
        VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        const VkCommandBufferInheritanceInfo* inheritance = nullptr
    ) noexcept -> std::expected<CommandRecorder, Vk::Error>;

    [[nodiscard]] auto Handle() const noexcept -> VkCommandBuffer { return _cmd; }
    [[nodiscard]] auto IsRecording() const noexcept -> bool { return _cmd != VK_NULL_HANDLE; }
    explicit operator bool() const noexcept { return IsRecording(); }

    [[nodiscard]] auto End() && noexcept -> std::expected<ExecutableCommands, Vk::Error>;
    void Abort() && noexcept;

  private:
    explicit CommandRecorder(VkCommandBuffer cmd) noexcept: _cmd(cmd) {}
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
};

static_assert(!std::is_default_constructible_v<CommandRecorder>);
static_assert(!std::is_copy_constructible_v<CommandRecorder> && !std::is_copy_assignable_v<CommandRecorder>);
static_assert(std::is_nothrow_move_constructible_v<CommandRecorder> && !std::is_move_assignable_v<CommandRecorder>);
static_assert(std::is_nothrow_destructible_v<CommandRecorder>);
static_assert(!std::is_default_constructible_v<ExecutableCommands>);
static_assert(!std::is_copy_constructible_v<ExecutableCommands> && !std::is_copy_assignable_v<ExecutableCommands>);
static_assert(!std::is_move_assignable_v<ExecutableCommands>);
static_assert(std::is_nothrow_move_constructible_v<ExecutableCommands> && std::is_nothrow_destructible_v<ExecutableCommands>);
static_assert(!std::is_constructible_v<ExecutableCommands, VkCommandBuffer>);

} // namespace ZHLN::Vk
