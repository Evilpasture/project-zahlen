// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/WindowInput.hpp>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>

namespace ZHLN {

class Window;
class PlatformHost;
class RenderContext;
class AudioContext;
class AssetManager;
namespace FS {
class FileSystemWatcher;
}
using FileSystemWatcher = FS::FileSystemWatcher;

class ZHLN_API Kernel {
  public:
    static auto Create(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<std::unique_ptr<Kernel>, ErrorCode>;
    ~Kernel();

    Kernel(const Kernel&)                    = delete;
    auto operator=(const Kernel&) -> Kernel& = delete;

    [[nodiscard]] auto IsRunning() const -> bool;

    [[nodiscard]] auto GetPlatformHost() noexcept -> PlatformHost&;
    [[nodiscard]] auto GetPlatformHost() const noexcept -> const PlatformHost&;

    [[nodiscard]] auto GetWindow() noexcept -> Window*;

    void ProcessEvents();

    auto AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver) -> Window*;
    void RemoveWindow(Window& window);

    [[nodiscard]] auto AcquireTarget() noexcept -> FrameOutcome<FrameTarget>;
    [[nodiscard]] auto AcquireTarget(Window& window) noexcept -> FrameOutcome<FrameTarget>;
    [[nodiscard]] auto GetAcquiredTarget() noexcept -> std::optional<FrameTarget>;
    [[nodiscard]] auto GetAcquiredTarget(Window& window) noexcept -> std::optional<FrameTarget>;

    auto GetRenderContext() -> RenderContext&;
    auto GetAudioContext() -> AudioContext&;
    auto GetAssetManager() -> AssetManager&;
    auto GetFileSystemWatcher() -> FileSystemWatcher&;

    [[nodiscard]] auto GetRenderConfig() const noexcept -> const RenderConfig&;

    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;
    void ProvokeDeviceLost();

  private:
    Kernel() = default;

    auto InitInternal(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<void, ErrorCode>;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
