// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/WindowInput.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ZHLN {

class PresentationTarget;
class Window;
class Kernel;

class ZHLN_API PlatformHost {
  public:
    struct Impl;


    [[nodiscard]] static auto CreateHeadless(uint32_t width, uint32_t height) -> PlatformHost;

    [[nodiscard]] static auto CreateTTY(uint32_t width, uint32_t height, const WindowInputReceiver& receiver) -> PlatformHost;

    [[nodiscard]] static auto
        CreateWindowed(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver) -> PlatformHost;

    PlatformHost() noexcept;
    ~PlatformHost() noexcept;

    PlatformHost(PlatformHost&& other) noexcept;
    auto operator=(PlatformHost&& other) noexcept -> PlatformHost&;

    PlatformHost(const PlatformHost&)                    = delete;
    auto operator=(const PlatformHost&) -> PlatformHost& = delete;

    [[nodiscard]] auto Valid() const noexcept -> bool;


    [[nodiscard]] auto IsRunning() const noexcept -> bool;

    void PollEvents() noexcept;

    void Close() const noexcept;


    [[nodiscard]] auto GetSize() const noexcept -> Extent2D;

    [[nodiscard]] auto HasNativeSurface() const noexcept -> bool;


    void               Focus() noexcept;
    [[nodiscard]] auto IsFocused() const noexcept -> bool;

    [[nodiscard]] auto WantsQuitProcess() const noexcept -> bool;
    void               AcknowledgeQuitProcess() noexcept;

    [[nodiscard]] auto GetClipboardText() const -> std::string;
    void               SetClipboardText(std::string_view text);

    void SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept;

    [[nodiscard]] auto AsWindow() noexcept -> Window*;
    [[nodiscard]] auto AsWindow() const noexcept -> const Window*;

  private:
    friend class Kernel;

    [[nodiscard]] auto Target() noexcept -> PresentationTarget&;
    [[nodiscard]] auto Target() const noexcept -> const PresentationTarget&;

    [[nodiscard]] auto TargetFor(Window& window) noexcept -> PresentationTarget&;

    std::unique_ptr<Impl> _impl;
};

}
