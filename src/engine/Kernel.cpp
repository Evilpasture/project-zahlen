// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Kernel.cpp
#include "EngineGlobals.hpp"
#include "Platform.hpp"
#include "tty/TTYBackend.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/FileSystemWatcher.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <cstdlib>
#include <filesystem>
#include <new>

namespace ZHLN {

// ============================================================================
// Kernel bootstrap errors
// Application bootstrap code branches on these specific failure reasons.
// ============================================================================

enum class KernelInitError : uint8_t {
    WindowCreationFailed       ZHLN_ANNOTATION(ZHLN::Description<"Window creation failed"> {}) = 1,
    TTYInitializationFailed    ZHLN_ANNOTATION(ZHLN::Description<"TTY initialization failed"> {}),
    RenderInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Render initialization failed"> {}),
    KernelAllocationFailed     ZHLN_ANNOTATION(ZHLN::Description<"Kernel instance allocation failed"> {}),
};

struct Kernel::Impl {
    // Declared first so it outlives every callback-owning client during normal
    // and partial-initialization teardown.
    std::unique_ptr<FileSystemWatcher>    fileSystemWatcher;
    std::unique_ptr<RenderContext>        renderContext;
    std::unique_ptr<AudioContext>         audioContext;
    std::unique_ptr<CreativeWorksManager> assetManager;
    std::vector<std::unique_ptr<Window>>  windows;
    std::vector<ViewportDesc>             extraViewports; // parallel to windows[1..]
    RenderConfig                          renderConfig;
    bool                                  glfwAcquired = false;
};

auto Kernel::Create(const RenderConfig& renderConfig, const WindowInputReceiver& inputReceiver) -> std::expected<std::unique_ptr<Kernel>, Error> {
    auto instance = std::unique_ptr<Kernel>(new (std::nothrow) Kernel());
    if (!instance) {
        return std::unexpected(KernelInitError::KernelAllocationFailed);
    }
    if (auto result = instance->InitInternal(renderConfig, inputReceiver); !result) {
        return std::unexpected(result.error());
    }
    return instance;
}

auto Kernel::InitInternal(const RenderConfig& cfg, const WindowInputReceiver& inputReceiver) -> std::expected<void, Error> {
    _impl                     = std::make_unique<Impl>();
    _impl->renderConfig       = cfg;
    _impl->fileSystemWatcher  = std::make_unique<FileSystemWatcher>();

    bool use_tty = false;

    if (cfg.headless) {
        // True headless mode: skip GLFW entirely. No display server is required.
        ZHLN::Log("[Kernel] Headless mode enabled. Skipping GLFW initialization.");
    } else {
        glfwSetErrorCallback([](int error, const char* description) -> void {
            ZHLN::Log("[GLFW Error] Code {}: {}", error, description ? description : "(null)");
        });

        if constexpr (isLinux) {
            // Detects both RenderDoc and NVIDIA Nsight Graphics (Nomad) launch environments
            if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr || std::getenv("NOMAD_VULKAN_LAYER") != nullptr ||
                std::getenv("NGFX_INJECTION") != nullptr) {
                glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            }
        }

        if (!AcquireGlfw()) {
            const char* desc = nullptr;
            int         err  = glfwGetError(&desc);
            if (desc != nullptr) {
                ZHLN::Log("[Kernel] glfwInit failed: ({}) {}", err, desc);
            }
            if (TTYBackend::IsSupported()) {
                ZHLN::Log("GLFW failed to initialize. Falling back to native TTY Display Mode.");
                use_tty = true;
            } else {
                return std::unexpected(KernelInitError::WindowCreationFailed);
            }
        } else {
            _impl->glfwAcquired = true;
        }
    }

    _impl->windows.push_back(
        std::make_unique<Window>(cfg.appName.data(), cfg.width, cfg.height, cfg.fullscreen, inputReceiver, use_tty, cfg.headless)
    );

    if (use_tty && _impl->windows.front()->GetTTYContext() == nullptr) {
        return std::unexpected(KernelInitError::TTYInitializationFailed);
    }

    InitRenderDocAPI();

    auto rc_res = RenderContext::Create(*_impl->windows.front(), cfg, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());

    _impl->audioContext = std::make_unique<AudioContext>();
    _impl->assetManager = std::make_unique<CreativeWorksManager>();

    if (std::filesystem::exists("data/base.pak")) {
        _impl->assetManager->MountPak("data/base.pak");
    } else if (std::filesystem::exists("build/data/base.pak")) {
        _impl->assetManager->MountPak("build/data/base.pak");
    } else {
        ZHLN::Log("WARNING: Could not find 'data/base.pak' in working directory or build/ folder!");
    }

    return {};
}

Kernel::~Kernel() {
    // Create can fail before _impl is built, and Kernel::Create deletes a
    // half-built kernel.
    if (_impl == nullptr) {
        return;
    }

    // GPU resources die before their windows; windows die before glfwTerminate.
    _impl->renderContext.reset();
    _impl->assetManager.reset();
    _impl->audioContext.reset();
    _impl->fileSystemWatcher.reset();
    _impl->windows.clear();

    // Process-global, refcounted like Jolt: extra windows and a second kernel
    // must not glfwTerminate under a window that is still open. Headless
    // kernels never acquire GLFW.
    if (_impl->glfwAcquired) {
        ReleaseGlfw();
    }
}

auto Kernel::IsRunning() const -> bool {
    return _impl->windows.front()->IsRunning();
}

auto Kernel::GetWindow() -> Window& {
    return *_impl->windows.front();
}

auto Kernel::GetWindow(size_t index) -> Window& {
    if (index >= _impl->windows.size()) {
        ZHLN::Panic("Kernel::GetWindow index {} out of range ({})", index, _impl->windows.size());
    }
    return *_impl->windows[index];
}

auto Kernel::WindowCount() const noexcept -> size_t {
    return _impl->windows.size();
}

void Kernel::ProcessEvents() {
    if (_impl->windows.front()->IsHeadless()) {
        // True headless mode: no windowing event queue to poll.
        return;
    }

    if (_impl->windows.front()->IsTTY()) {
        // TTY path uses the same WindowInputReceiver callbacks as GLFW
        TTYBackend::ProcessEvents(_impl->windows.front()->GetTTYContext(), _impl->windows.front()->GetInputReceiver());
        return;
    }

    glfwPollEvents();

    // Super+Q on any focused window ends the process. Super+W already called
    // Window::Close on that window in the key callback.
    for (const auto& window: _impl->windows) {
        if (window != nullptr && window->WantsQuitProcess()) {
            window->AcknowledgeQuitProcess();
            _impl->windows.front()->Close();
            break;
        }
    }
}

auto Kernel::AddWindow(
    const String32&            title,
    uint32_t                   width,
    uint32_t                   height,
    bool                       fullscreen,
    const WindowInputReceiver& receiver,
    ViewportMode               mode,
    Entity                     camera
) -> Window* {
    if (_impl->windows.empty() || !_impl->glfwAcquired || _impl->windows.front()->IsHeadless() || _impl->windows.front()->IsTTY()) {
        ZHLN::Log("[Kernel] AddWindow requires an initialized GLFW session");
        return nullptr;
    }

    auto window = std::make_unique<Window>(title, width, height, fullscreen, receiver, false, false);
    if (window->GetNativeHandle() == nullptr) {
        ZHLN::Log("[Kernel] AddWindow: OS window creation failed");
        return nullptr;
    }
    Window*      raw = window.get();
    ViewportDesc desc {.mode = mode, .camera = camera};
    _impl->windows.push_back(std::move(window));
    _impl->extraViewports.push_back(desc);
    if (_impl->renderContext != nullptr) {
        if (auto presented = _impl->renderContext->AddViewport(*raw, desc); !presented) {
            ZHLN::Log("[Kernel] AddWindow: extra viewport failed ({})", presented.error());
            _impl->windows.pop_back();
            _impl->extraViewports.pop_back();
            return nullptr;
        }
    }
    return raw;
}

void Kernel::RemoveWindow(Window& window) {
    if (_impl->windows.empty() || _impl->windows.front().get() == &window) {
        return;
    }
    size_t extraIdx = 0;
    for (size_t i = 1; i < _impl->windows.size(); ++i) {
        if (_impl->windows[i].get() == &window) {
            extraIdx = i - 1;
            break;
        }
    }
    if (_impl->renderContext != nullptr) {
        if (auto removed = _impl->renderContext->RemoveViewport(window); !removed) {
            ZHLN::Log("[Kernel] RemoveWindow: extra viewport teardown failed ({})", removed.error());
        }
    }
    std::erase_if(_impl->windows, [&](const std::unique_ptr<Window>& owned) -> bool { return owned.get() == &window; });
    if (extraIdx < _impl->extraViewports.size()) {
        _impl->extraViewports.erase(_impl->extraViewports.begin() + static_cast<std::ptrdiff_t>(extraIdx));
    }
}

auto Kernel::GetRenderContext() -> RenderContext& {
    return *_impl->renderContext;
}
auto Kernel::GetAudioContext() -> AudioContext& {
    return *_impl->audioContext;
}
auto Kernel::GetAssetManager() -> CreativeWorksManager& {
    return *_impl->assetManager;
}
auto Kernel::GetFileWatcher() -> FileSystemWatcher& {
    return *_impl->fileSystemWatcher;
}

auto Kernel::GetRenderConfig() const noexcept -> const RenderConfig& {
    return _impl->renderConfig;
}

auto Kernel::HandleDeviceLost() noexcept -> std::expected<void, Error> {
    _impl->renderContext->OnDeviceLost();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(*_impl->windows.front(), _impl->renderConfig, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    for (size_t i = 1; i < _impl->windows.size(); ++i) {
        ViewportDesc desc {};
        if (i - 1 < _impl->extraViewports.size()) {
            desc = _impl->extraViewports[i - 1];
        }
        if (auto presented = _impl->renderContext->AddViewport(*_impl->windows[i], desc); !presented) {
            ZHLN::Log("[Kernel] HandleDeviceLost: extra viewport {} failed ({})", i, presented.error());
        }
    }
    return {};
}

void Kernel::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

} // namespace ZHLN
