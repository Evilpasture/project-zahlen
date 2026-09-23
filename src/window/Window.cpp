// src/window/Window.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The desktop presentation target. This is the translation unit that owns GLFW:
// it opens the window, pumps its events, and translates what GLFW reports about
// the platform into the opaque NativeSurfaceHandle the RHI reads. Nothing here
// names a Vulkan type -- the handle leaves this subsystem as void* members of a
// variant, and src/vulkan/presentation/Surface.cpp is what turns them into a
// VkSurfaceKHR.
#include "WindowInternal.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window.hpp>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>

// Native accessors, gated on the backends GLFW was actually built with.
//
// glfw3native.h includes the platform's own headers -- <X11/Xlib.h> and
// <X11/extensions/Xrandr.h> for X11, <wayland-client.h> for Wayland -- so asking
// for a backend unconditionally would make this translation unit need
// development packages the build explicitly supports being without: the root
// CMakeLists probes for them and builds GLFW with only what it found, falling
// back to GLFW's null platform when there is neither (see GLFW_BUILD_X11 /
// GLFW_BUILD_WAYLAND there). It mirrors that decision here as
// ZHLN_WINDOW_NATIVE_*, so this file includes exactly what the linked GLFW can
// answer for.
//
// glfw3.h itself comes in through WindowInternal.hpp and pulls no Vulkan headers
// with it: GLFW_INCLUDE_VULKAN is not defined anywhere in this subsystem.
#if defined(ZHLN_WINDOW_NATIVE_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(ZHLN_WINDOW_NATIVE_WAYLAND) || defined(ZHLN_WINDOW_NATIVE_X11)
#if defined(ZHLN_WINDOW_NATIVE_WAYLAND)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#if defined(ZHLN_WINDOW_NATIVE_X11)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
#endif

namespace ZHLN {

namespace {

[[maybe_unused]] consteval auto KeyCodeToGLFW(KeyCode key) noexcept -> int {
    using enum KeyCode;
    switch (key) {
        // Numbers 0 - 9
        case Num0:
            return GLFW_KEY_0;
        case Num1:
            return GLFW_KEY_1;
        case Num2:
            return GLFW_KEY_2;
        case Num3:
            return GLFW_KEY_3;
        case Num4:
            return GLFW_KEY_4;
        case Num5:
            return GLFW_KEY_5;
        case Num6:
            return GLFW_KEY_6;
        case Num7:
            return GLFW_KEY_7;
        case Num8:
            return GLFW_KEY_8;
        case Num9:
            return GLFW_KEY_9;

        // Letters A - Z
        case A:
            return GLFW_KEY_A;
        case B:
            return GLFW_KEY_B;
        case C:
            return GLFW_KEY_C;
        case D:
            return GLFW_KEY_D;
        case E:
            return GLFW_KEY_E;
        case F:
            return GLFW_KEY_F;
        case G:
            return GLFW_KEY_G;
        case H:
            return GLFW_KEY_H;
        case I:
            return GLFW_KEY_I;
        case J:
            return GLFW_KEY_J;
        case K:
            return GLFW_KEY_K;
        case L:
            return GLFW_KEY_L;
        case M:
            return GLFW_KEY_M;
        case N:
            return GLFW_KEY_N;
        case O:
            return GLFW_KEY_O;
        case P:
            return GLFW_KEY_P;
        case Q:
            return GLFW_KEY_Q;
        case R:
            return GLFW_KEY_R;
        case S:
            return GLFW_KEY_S;
        case T:
            return GLFW_KEY_T;
        case U:
            return GLFW_KEY_U;
        case V:
            return GLFW_KEY_V;
        case W:
            return GLFW_KEY_W;
        case X:
            return GLFW_KEY_X;
        case Y:
            return GLFW_KEY_Y;
        case Z:
            return GLFW_KEY_Z;

        // Function Keys F1 - F12
        case F1:
            return GLFW_KEY_F1;
        case F2:
            return GLFW_KEY_F2;
        case F3:
            return GLFW_KEY_F3;
        case F4:
            return GLFW_KEY_F4;
        case F5:
            return GLFW_KEY_F5;
        case F6:
            return GLFW_KEY_F6;
        case F7:
            return GLFW_KEY_F7;
        case F8:
            return GLFW_KEY_F8;
        case F9:
            return GLFW_KEY_F9;
        case F10:
            return GLFW_KEY_F10;
        case F11:
            return GLFW_KEY_F11;
        case F12:
            return GLFW_KEY_F12;

        // Modifiers
        case LShift:
            return GLFW_KEY_LEFT_SHIFT;
        case RShift:
            return GLFW_KEY_RIGHT_SHIFT;
        case LControl:
            return GLFW_KEY_LEFT_CONTROL;
        case RControl:
            return GLFW_KEY_RIGHT_CONTROL;
        case LAlt:
            return GLFW_KEY_LEFT_ALT;
        case RAlt:
            return GLFW_KEY_RIGHT_ALT;
        case LSuper:
            return GLFW_KEY_LEFT_SUPER;
        case RSuper:
            return GLFW_KEY_RIGHT_SUPER;

        // Navigation & Editing
        case Space:
            return GLFW_KEY_SPACE;
        case Escape:
            return GLFW_KEY_ESCAPE;
        case Enter:
            return GLFW_KEY_ENTER;
        case Backspace:
            return GLFW_KEY_BACKSPACE;
        case Tab:
            return GLFW_KEY_TAB;
        case Delete:
            return GLFW_KEY_DELETE;

        // Arrow Keys
        case Up:
            return GLFW_KEY_UP;
        case Down:
            return GLFW_KEY_DOWN;
        case Left:
            return GLFW_KEY_LEFT;
        case Right:
            return GLFW_KEY_RIGHT;

        // Line / page navigation
        case Home:
            return GLFW_KEY_HOME;
        case End:
            return GLFW_KEY_END;
        case PageUp:
            return GLFW_KEY_PAGE_UP;
        case PageDown:
            return GLFW_KEY_PAGE_DOWN;

        default:
            return GLFW_KEY_UNKNOWN;
    }
}

consteval auto BuildGLFWToKeyCodeTable() noexcept {
    std::array<KeyCode, GLFW_KEY_LAST + 1> table {};
    table.fill(KeyCode::Unknown);

    ZHLN::Reflect::ForEachEnumerator<KeyCode>([&]<KeyCode Key>() -> auto {
        static constexpr int glfwCode = KeyCodeToGLFW(Key);
        if constexpr (glfwCode >= 0 && glfwCode <= GLFW_KEY_LAST) {
            table[glfwCode] = Key;
        }
    });

    return table;
}

auto MapGLFWKey(int key) noexcept -> KeyCode {
    static constexpr auto Table = BuildGLFWToKeyCodeTable();
    if (key >= 0 && key <= GLFW_KEY_LAST) [[likely]] {
        return Table[key];
    }
    return KeyCode::Unknown;
}

// Linux evdev KEY_LEFTMETA / KEY_RIGHTMETA. GLFW Wayland scancodes are evdev
// codes; Hyprland often never delivers Super as GLFW_KEY_* because it is the
// compositor modifier.
[[nodiscard]] auto IsSuperScancode(int scancode) noexcept -> bool {
#if defined(__linux__)
    return scancode == 125 || scancode == 126;
#else
    (void) scancode;
    return false;
#endif
}

[[nodiscard]] auto SuperHeld(GLFWwindow* win, int mods, bool sticky) noexcept -> bool {
    if ((mods & GLFW_MOD_SUPER) != 0 || sticky) {
        return true;
    }
    return glfwGetKey(win, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
}

[[nodiscard]] auto ControlHeld(GLFWwindow* win, int mods) noexcept -> bool {
    if ((mods & GLFW_MOD_CONTROL) != 0) {
        return true;
    }
    return glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
}

// Reads a dropped file from disk into a FileDrop. Tolerates missing/unreadable
// paths by returning a struct with empty data (the caller may skip it).
auto ReadDroppedFile(const char* path) -> FileDrop {
    FileDrop result;
    if (path == nullptr) {
        return result;
    }
    result.sourcePath = path;

    const std::filesystem::path p(path);
    result.fileName = p.filename().string();

    std::string ext = p.extension().string();
    for (char& c: ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    result.format = ext;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return result;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return result;
    }
    file.seekg(0, std::ios::beg);
    result.data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(result.data.data()), size)) {
        result.data.clear();
        return result;
    }
    result.byteSize = static_cast<uint64_t>(size);
    return result;
}

// What the OS gave this window, as the variant the RHI visits. One branch per
// platform GLFW can run on; the branch that is not compiled away is chosen by
// the preprocessor, not at runtime, so a build carries no platform's handle
// type it cannot produce.
[[nodiscard]] auto QueryNativeTarget(GLFWwindow* handle) noexcept -> NativeSurfaceVariant {
#if defined(ZHLN_WINDOW_NATIVE_WIN32)
    return Win32Target {.hwnd = glfwGetWin32Window(handle), .hinstance = GetModuleHandleW(nullptr)};
#elif defined(__APPLE__)
    // macOS has no native Vulkan WSI: a windowed session there renders
    // offscreen and the host-blit plugin presents through its own OpenGL
    // window. A caller that supplied a CAMetalLayer could fill this in; the
    // GLFW path deliberately does not, and the RHI reads a null layer as "no
    // WSI surface", which is what it already asked for.
    (void) handle;
    return CocoaTarget {};
#elif defined(ZHLN_WINDOW_NATIVE_WAYLAND) || defined(ZHLN_WINDOW_NATIVE_X11)
#if defined(ZHLN_WINDOW_NATIVE_WAYLAND)
    // glfwGetPlatform() is the authority here, not the environment: it reports
    // the protocol GLFW actually connected to, which on a compositor running
    // both is the one that decides which WSI extension the instance needs.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return WaylandTarget {.display = glfwGetWaylandDisplay(), .surface = glfwGetWaylandWindow(handle)};
    }
#endif
#if defined(ZHLN_WINDOW_NATIVE_X11)
    // X11, including XWayland: an X client either way, so the Xlib display and
    // the window's XID are what the RHI needs to build the surface.
    return X11Target {.display = glfwGetX11Display(), .window = static_cast<unsigned long>(glfwGetX11Window(handle))};
#else
    // A Wayland-only build that is not on Wayland has no handle to publish.
    // Unreachable in practice -- such a session cannot have created a window at
    // all, and RebuildNativeSurface hands out an empty handle for that.
    (void) handle;
    return HeadlessTarget {};
#endif
#else
    // No native backend in this build: nothing to publish, and a consumer reads
    // it as "no surface here".
    (void) handle;
    return HeadlessTarget {};
#endif
}

// The two dynamic halves of the presentation target this window composes.
// Plain functions on an opaque userdata rather than overrides on an interface:
// the target does not know what a GLFWwindow is, and this is the only file in
// the tree that does.
auto QueryFramebufferExtent(void* userdata) noexcept -> Extent2D {
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(userdata), &w, &h);
    return {.width = static_cast<uint32_t>(w), .height = static_cast<uint32_t>(h)};
}

void RequestWindowClose(void* userdata) noexcept {
    if (userdata != nullptr) {
        glfwSetWindowShouldClose(static_cast<GLFWwindow*>(userdata), GLFW_TRUE);
    }
}

} // namespace

void Window::RebuildNativeSurface() noexcept {
    // A Window is always a desktop window now: the headless and KMS/DRM sessions
    // that used to be flags on this class are their own PlatformHost
    // implementations, with their own presentation targets, and never construct
    // one of these. What is left is the one question this has to answer.
    if (_impl->handle == nullptr) {
        // A window that failed to open has no descriptor to hand over. Valid()
        // is false, and a consumer reports "unsupported" instead of building a
        // surface from a null handle.
        _impl->target.SetNativeSurface(NativeSurfaceHandle());
        return;
    }
    _impl->target.SetNativeSurface(NativeSurfaceHandle(std::make_unique<NativeSurfaceHandle::Impl>(QueryNativeTarget(_impl->handle))));
}

Window::Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver): _impl(std::make_unique<Impl>()) {
    _impl->receiver = receiver;

    // No GLFW bootstrap here: the error callback and the init hints belong to
    // AcquireGlfw(), which owns the process-global init this window is being
    // created inside. A second window must not re-run them.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWmonitor* monitor = nullptr;
    if (fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        if (monitor != nullptr) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwWindowHint(GLFW_RED_BITS, mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

            width  = (width == 0) ? static_cast<uint32_t>(mode->width) : width;
            height = (height == 0) ? static_cast<uint32_t>(mode->height) : height;
        }
    }

    _impl->handle = glfwCreateWindow(width, height, title.c_str(), monitor, nullptr);
    glfwSetWindowUserPointer(_impl->handle, this);

    if (_impl->handle != nullptr) {
        glfwShowWindow(_impl->handle);
        glfwPollEvents();
    }

    // Register window-level callbacks routing through the generic receiver
    glfwSetKeyCallback(_impl->handle, [](GLFWwindow* win, int key, int scancode, int action, int mods) -> void {
        auto*   self   = static_cast<Window*>(glfwGetWindowUserPointer(win));
        KeyCode mapped = MapGLFWKey(key);

        if (key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER || IsSuperScancode(scancode)) {
            self->_impl->superDown = (action != GLFW_RELEASE);
        }

        // Super+Q / Ctrl+Q quits; Super+W / Ctrl+W closes this window.
        // Hyprland binds Super as the compositor mod, so GLFW_MOD_SUPER is
        // often missing; Ctrl is what Linux apps actually receive. Press
        // only (not repeat).
        if (action == GLFW_PRESS) {
            const bool chord = SuperHeld(win, mods, self->_impl->superDown) || ControlHeld(win, mods);
            if (chord && key == GLFW_KEY_Q) {
                self->_impl->quitProcess = true;
            } else if (chord && key == GLFW_KEY_W) {
                self->Close();
            }
        }

        if (self->_impl->receiver.onKey) {
            bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);
            self->_impl->receiver.onKey(self->_impl->receiver.userdata, mapped, pressed);
        }
    });

    glfwSetWindowCloseCallback(_impl->handle, [](GLFWwindow* win) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self != nullptr) {
            self->Close();
        }
    });

    glfwSetMouseButtonCallback(_impl->handle, [](GLFWwindow* win, int button, int action, int /*mods*/) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self->_impl->receiver.onKey) {
            bool pressed = (action == GLFW_PRESS);
            if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::RButton, pressed);
            } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::LButton, pressed);
            } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::MButton, pressed);
            }
        }
    });

    glfwSetCursorPosCallback(_impl->handle, [](GLFWwindow* win, double xpos, double ypos) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self->_impl->receiver.onMouseMove) {
            int winWidth  = 0;
            int winHeight = 0;
            glfwGetWindowSize(win, &winWidth, &winHeight);

            int fbWidth  = 0;
            int fbHeight = 0;
            glfwGetFramebufferSize(win, &fbWidth, &fbHeight);

            float scaleX = (winWidth > 0) ? static_cast<float>(fbWidth) / static_cast<float>(winWidth) : 1.0f;
            float scaleY = (winHeight > 0) ? static_cast<float>(fbHeight) / static_cast<float>(winHeight) : 1.0f;

            self->_impl->receiver.onMouseMove(self->_impl->receiver.userdata, static_cast<float>(xpos) * scaleX, static_cast<float>(ypos) * scaleY);
        }
    });

    glfwSetFramebufferSizeCallback(_impl->handle, [](GLFWwindow* win, int fbWidth, int fbHeight) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self->_impl->receiver.onResize) {
            self->_impl->receiver.onResize(
                self->_impl->receiver.userdata, {.width = static_cast<uint32_t>(fbWidth), .height = static_cast<uint32_t>(fbHeight)}
            );
        }
    });

    glfwSetScrollCallback(_impl->handle, [](GLFWwindow* win, double /*xoffset*/, double yoffset) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self->_impl->receiver.onMouseScroll) {
            self->_impl->receiver.onMouseScroll(self->_impl->receiver.userdata, static_cast<float>(yoffset));
        }
    });

    glfwSetCharCallback(_impl->handle, [](GLFWwindow* win, unsigned int codepoint) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self->_impl->receiver.onChar) {
            self->_impl->receiver.onChar(self->_impl->receiver.userdata, codepoint);
        }
    });

    // Abstract file-drop handler: read every dropped file into a FileDrop and
    // forward the batch to the registered onFileDrop receiver callback.
    glfwSetDropCallback(_impl->handle, [](GLFWwindow* win, int count, const char** paths) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self == nullptr || self->_impl->receiver.onFileDrop == nullptr || paths == nullptr || count <= 0) {
            return;
        }

        std::vector<FileDrop> drops;
        drops.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            FileDrop d = ReadDroppedFile(paths[i]);
            if (!d.data.empty()) {
                drops.push_back(std::move(d));
            }
        }
        if (!drops.empty()) {
            self->_impl->receiver.onFileDrop(self->_impl->receiver.fileDropUserdata, drops.data(), static_cast<uint32_t>(drops.size()));
        }
    });

    // The renderer's handle on this window. Installed before the descriptor is
    // published so that from here on it only ever changes in place: the
    // destination registry keys on this object's address.
    _impl->target = PresentationTarget::ForWindow(static_cast<void*>(_impl->handle), &QueryFramebufferExtent, &RequestWindowClose);

    // The window exists now -- or did not open, which the empty handle says.
    // Publish what the OS gave us: the renderer reads this once, at instance and
    // surface creation, so building it here costs the frame path nothing.
    RebuildNativeSurface();
}

Window::~Window() {
    if (_impl->handle != nullptr) {
        glfwDestroyWindow(_impl->handle);
    }
}

auto Window::IsRunning() const -> bool {
    return _impl->handle != nullptr && glfwWindowShouldClose(_impl->handle) == 0;
}

void Window::ProcessEvents() {
}

// --- The facade
//
// What a client of the engine sees. None of this names an internal type, which
// is why <Zahlen/Window.hpp> can declare it without this header.

auto Window::GetSize() const -> Extent2D {
    return _impl->target.GetFramebufferExtent();
}

void Window::SetSize(uint32_t width, uint32_t height) noexcept {
    // Advisory only: the compositor owns a window's real size, so this is
    // recorded and GetFramebufferExtent keeps asking GLFW. Callers that need it
    // to stick are the headless ones, which have no compositor to ask.
    _impl->target.SetFramebufferExtent(width, height);
}

// Private: the windowing subsystem's only door to this window's target, which
// PlatformHost asks through for the windowed case of the session's target. The
// engine asks PlatformHost instead and never names a window's state -- see the
// friend declaration in <Zahlen/Window.hpp>.
auto Window::Target() noexcept -> PresentationTarget& {
    return _impl->target;
}

void Window::Focus() {
    if (_impl->handle != nullptr) {
        glfwFocusWindow(_impl->handle);
    }
}

auto Window::IsFocused() const -> bool {
    if (_impl->handle == nullptr) {
        return false;
    }
    return glfwGetWindowAttrib(_impl->handle, GLFW_FOCUSED) != 0;
}

auto Window::WantsQuitProcess() const noexcept -> bool {
    return _impl->quitProcess;
}

void Window::AcknowledgeQuitProcess() noexcept {
    _impl->quitProcess = false;
}

auto Window::GetNativeHandle() const -> void* {
    return _impl->handle;
}

auto Window::GetPlatform() const noexcept -> WindowPlatform {
    const auto x11IsXWayland = []() noexcept -> bool {
        if (std::getenv("WAYLAND_DISPLAY") != nullptr || std::getenv("WAYLAND_SOCKET") != nullptr) {
            return true;
        }
        if (const char* session = std::getenv("XDG_SESSION_TYPE"); session != nullptr) {
            return std::strcmp(session, "wayland") == 0;
        }
        return false;
    };

#if defined(GLFW_PLATFORM_WAYLAND)
    switch (glfwGetPlatform()) {
        case GLFW_PLATFORM_WAYLAND:
            return WindowPlatform::Wayland;
        case GLFW_PLATFORM_X11:
            return x11IsXWayland() ? WindowPlatform::XWayland : WindowPlatform::X11;
        case GLFW_PLATFORM_WIN32:
            return WindowPlatform::Win32;
        case GLFW_PLATFORM_COCOA:
            return WindowPlatform::Cocoa;
        default:
            return WindowPlatform::Unknown;
    }
#else
    (void) x11IsXWayland;
    return WindowPlatform::Unknown;
#endif
}

void Window::Close() const noexcept {
    // Through the target rather than a method of its own: the target's close
    // hook is this window's glfwSetWindowShouldClose, so there is one way to end
    // a session and the renderer can reach it too (PresentationTarget::Close).
    _impl->target.Close();
}

void Window::CaptureMouse(bool captured) {
    if (_impl->handle != nullptr) {
        glfwSetInputMode(_impl->handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
}

auto Window::GetInputReceiver() const noexcept -> const WindowInputReceiver& {
    return _impl->receiver;
}

void Window::SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept {
    _impl->receiver.onFileDrop       = handler;
    _impl->receiver.fileDropUserdata = userdata;
}

auto Window::GetClipboardText() const -> std::string {
    if (_impl->handle != nullptr) {
        // GLFW returns nullptr (and reports GLFW_FORMAT_UNAVAILABLE) when the
        // clipboard is empty or non-text; that is an empty paste, not an error.
        const char* text = glfwGetClipboardString(_impl->handle);
        return (text != nullptr) ? std::string(text) : std::string();
    }
    return _impl->localClipboard;
}

void Window::SetClipboardText(std::string_view text) {
    // Always keep the local copy: it is the fallback when GLFW's clipboard is
    // unavailable (a Wayland compositor with no focus, an X server without
    // a selection owner).
    _impl->localClipboard.assign(text);
    if (_impl->handle != nullptr) {
        glfwSetClipboardString(_impl->handle, _impl->localClipboard.c_str());
    }
}

} // namespace ZHLN
