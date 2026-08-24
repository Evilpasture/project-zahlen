// src/engine/Window.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Platform.hpp"
#include "TTYBackend.hpp"
#include "WindowInternal.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Window.hpp>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>

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

} // namespace

Window::Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver, bool useTTY, bool headless):
    _impl(std::make_unique<Impl>()) {
    _impl->receiver = receiver;
    _impl->is_tty   = useTTY;
    _impl->headless = headless;

    if (_impl->headless) {
        // True headless mode: bypass GLFW entirely. No display server, no window,
        // no surface. Running state is managed internally.
        _impl->handle     = nullptr;
        _impl->is_running = true;
        _impl->width      = width;
        _impl->height     = height;
        return;
    }

    if (_impl->is_tty) {
        _impl->width       = width;
        _impl->height      = height;
        _impl->tty_context = TTYBackend::Init(width, height);
    } else {
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
        glfwSetKeyCallback(_impl->handle, [](GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) -> void {
            auto*   self   = static_cast<Window*>(glfwGetWindowUserPointer(win));
            KeyCode mapped = MapGLFWKey(key);

            if (self->_impl->receiver.onKey) {
                bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, mapped, pressed);
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
    }
}

Window::~Window() {
    if (_impl->is_tty) {
        TTYBackend::Shutdown(_impl->tty_context);
    } else if (_impl->handle != nullptr) {
        glfwDestroyWindow(_impl->handle);
    }
}

auto Window::IsRunning() const -> bool {
    if (_impl->headless) {
        return _impl->is_running;
    }
    if (_impl->is_tty) {
        return TTYBackend::IsRunning(_impl->tty_context);
    }
    return glfwWindowShouldClose(_impl->handle) == 0;
}

void Window::ProcessEvents() {
}

auto Window::GetSize() const -> Extent2D {
    if (_impl->headless || _impl->is_tty) {
        return {.width = _impl->width, .height = _impl->height};
    }

    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(_impl->handle, &w, &h);
    return {.width = static_cast<uint32_t>(w), .height = static_cast<uint32_t>(h)};
}

void Window::SetSize(uint32_t width, uint32_t height) noexcept {
    _impl->width  = width;
    _impl->height = height;
}

void Window::Focus() {
    if (!_impl->is_tty && _impl->handle != nullptr) {
        glfwFocusWindow(_impl->handle);
    }
}

auto Window::GetNativeHandle() const -> void* {
    return _impl->handle;
}

void Window::Close() {
    if (_impl->headless) {
        _impl->is_running = false;
        return;
    }
    if (!_impl->is_tty && _impl->handle != nullptr) {
        glfwSetWindowShouldClose(_impl->handle, GLFW_TRUE);
    }
}

void Window::CaptureMouse(bool captured) {
    if (!_impl->is_tty && (_impl->handle != nullptr)) {
        glfwSetInputMode(_impl->handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
}

auto Window::IsTTY() const -> bool {
    return _impl->is_tty;
}

auto Window::IsHeadless() const -> bool {
    return _impl->headless;
}

auto Window::GetTTYContext() const -> void* {
    return _impl->tty_context;
}

auto Window::GetInputReceiver() const noexcept -> const WindowInputReceiver& {
    return _impl->receiver;
}

void Window::SetFileDropHandler(void (*handler)(void* userdata, const FileDrop* files, uint32_t count), void* userdata) noexcept {
    _impl->receiver.onFileDrop        = handler;
    _impl->receiver.fileDropUserdata  = userdata;
}

auto Window::ReinitTTY() -> bool {
    if (_impl->is_tty && _impl->tty_context == nullptr) {
        _impl->tty_context = TTYBackend::Init(_impl->width, _impl->height);
        return _impl->tty_context != nullptr;
    }
    return false;
}

} // namespace ZHLN
