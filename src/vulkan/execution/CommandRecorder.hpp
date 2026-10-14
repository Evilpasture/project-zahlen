// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

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

class CommandRecorder {
  public:
    CommandRecorder() = delete;
    ~CommandRecorder() noexcept = default;
    CommandRecorder(const CommandRecorder&) = delete;
    auto operator=(const CommandRecorder&) -> CommandRecorder& = delete;
    CommandRecorder(CommandRecorder&& other) noexcept: _cmd(std::exchange(other._cmd, VK_NULL_HANDLE)) {}

    auto operator=(CommandRecorder&&) -> CommandRecorder& = delete;

    [[nodiscard]] static auto Begin(
        VkCommandBuffer cmd,
        VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        const VkCommandBufferInheritanceInfo* inheritance = nullptr
    ) noexcept -> std::expected<CommandRecorder, ErrorCode>;

    [[nodiscard]] auto Handle() const noexcept -> VkCommandBuffer { return _cmd; }
    [[nodiscard]] auto IsRecording() const noexcept -> bool { return _cmd != VK_NULL_HANDLE; }
    explicit operator bool() const noexcept { return IsRecording(); }

    [[nodiscard]] auto End() && noexcept -> std::expected<ExecutableCommands, ErrorCode>;
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
