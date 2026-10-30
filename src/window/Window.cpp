// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "WindowInternal.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window/Window.hpp>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>

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

        case Up:
            return GLFW_KEY_UP;
        case Down:
            return GLFW_KEY_DOWN;
        case Left:
            return GLFW_KEY_LEFT;
        case Right:
            return GLFW_KEY_RIGHT;

        case Home:
            return GLFW_KEY_HOME;
        case End:
            return GLFW_KEY_END;
        case PageUp:
            return GLFW_KEY_PAGE_UP;
        case PageDown:
            return GLFW_KEY_PAGE_DOWN;
        case Insert:
            return GLFW_KEY_INSERT;
        case CapsLock:
            return GLFW_KEY_CAPS_LOCK;
        case Pause:
            return GLFW_KEY_PAUSE;
        case PrintScreen:
            return GLFW_KEY_PRINT_SCREEN;
        case Minus:
            return GLFW_KEY_MINUS;
        case Equal:
            return GLFW_KEY_EQUAL;
        case LeftBracket:
            return GLFW_KEY_LEFT_BRACKET;
        case RightBracket:
            return GLFW_KEY_RIGHT_BRACKET;
        case Backslash:
            return GLFW_KEY_BACKSLASH;
        case Semicolon:
            return GLFW_KEY_SEMICOLON;
        case Apostrophe:
            return GLFW_KEY_APOSTROPHE;
        case GraveAccent:
            return GLFW_KEY_GRAVE_ACCENT;
        case Comma:
            return GLFW_KEY_COMMA;
        case Period:
            return GLFW_KEY_PERIOD;
        case Slash:
            return GLFW_KEY_SLASH;

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

[[nodiscard]] auto QueryNativeTarget(GLFWwindow* handle) noexcept -> NativeSurfaceVariant {
#if defined(ZHLN_WINDOW_NATIVE_WIN32)
    return Win32Target {.hwnd = glfwGetWin32Window(handle), .hinstance = GetModuleHandleW(nullptr)};
#elif defined(__APPLE__)
    (void) handle;
    return CocoaTarget {};
#elif defined(ZHLN_WINDOW_NATIVE_WAYLAND) || defined(ZHLN_WINDOW_NATIVE_X11)
#if defined(ZHLN_WINDOW_NATIVE_WAYLAND)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return WaylandTarget {.display = glfwGetWaylandDisplay(), .surface = glfwGetWaylandWindow(handle)};
    }
#endif
#if defined(ZHLN_WINDOW_NATIVE_X11)
    return X11Target {.display = glfwGetX11Display(), .window = static_cast<unsigned long>(glfwGetX11Window(handle))};
#else
    (void) handle;
    return HeadlessTarget {};
#endif
#else
    (void) handle;
    return HeadlessTarget {};
#endif
}

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

}

void Window::RebuildNativeSurface() noexcept {
    if (_impl->handle == nullptr) {
        _impl->target.SetNativeSurface(NativeSurfaceHandle());
        return;
    }
    _impl->target.SetNativeSurface(NativeSurfaceHandle(QueryNativeTarget(_impl->handle)));
}

Window::Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver): _impl(std::make_unique<Impl>()) {
    _impl->receiver = receiver;

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

    glfwSetKeyCallback(_impl->handle, [](GLFWwindow* win, int key, int scancode, int action, int mods) -> void {
        auto*   self   = static_cast<Window*>(glfwGetWindowUserPointer(win));
        KeyCode mapped = MapGLFWKey(key);

        if (key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER || IsSuperScancode(scancode)) {
            self->_impl->superDown = (action != GLFW_RELEASE);
        }

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

    glfwSetMouseButtonCallback(_impl->handle, [](GLFWwindow* win, int button, int action, int ) -> void {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
        if (self->_impl->receiver.onKey) {
            bool pressed = (action == GLFW_PRESS);
            if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::RButton, pressed);
            } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::LButton, pressed);
            } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::MButton, pressed);
            } else if (button == GLFW_MOUSE_BUTTON_4) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::XButton1, pressed);
            } else if (button == GLFW_MOUSE_BUTTON_5) {
                self->_impl->receiver.onKey(self->_impl->receiver.userdata, KeyCode::XButton2, pressed);
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

    glfwSetScrollCallback(_impl->handle, [](GLFWwindow* win, double , double yoffset) -> void {
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

    _impl->target = PresentationTarget::ForWindow(static_cast<void*>(_impl->handle), &QueryFramebufferExtent, &RequestWindowClose);

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
    const auto& rx = _impl->receiver;
    if (rx.onGamepadConnection == nullptr && rx.onGamepadState == nullptr) {
        return;
    }

    for (uint8_t slot = 0; slot < kMaxGamepads; ++slot) {
        const int  jid     = GLFW_JOYSTICK_1 + static_cast<int>(slot);
        const bool present = glfwJoystickPresent(jid) == GLFW_TRUE && glfwJoystickIsGamepad(jid) == GLFW_TRUE;

        if (present != _impl->gamepadConnected[slot]) {
            _impl->gamepadConnected[slot] = present;
            if (rx.onGamepadConnection != nullptr) {
                const char* rawName = present ? glfwGetGamepadName(jid) : nullptr;
                rx.onGamepadConnection(rx.userdata, slot, present, rawName != nullptr ? std::string_view(rawName) : std::string_view {});
            }
        }

        if (!present || rx.onGamepadState == nullptr) {
            continue;
        }

        GLFWgamepadstate gp {};
        if (glfwGetGamepadState(jid, &gp) != GLFW_TRUE) {
            continue;
        }

        GamepadRawSample sample {};
        sample.buttons[static_cast<size_t>(GamepadButton::South)]       = gp.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::East)]        = gp.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::West)]        = gp.buttons[GLFW_GAMEPAD_BUTTON_X] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::North)]       = gp.buttons[GLFW_GAMEPAD_BUTTON_Y] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::LeftBumper)]  = gp.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::RightBumper)] = gp.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::Back)]        = gp.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::Start)]       = gp.buttons[GLFW_GAMEPAD_BUTTON_START] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::Guide)]       = gp.buttons[GLFW_GAMEPAD_BUTTON_GUIDE] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::LeftThumb)]   = gp.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::RightThumb)]  = gp.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::DPadUp)]      = gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::DPadRight)]   = gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::DPadDown)]    = gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] == GLFW_PRESS;
        sample.buttons[static_cast<size_t>(GamepadButton::DPadLeft)]    = gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] == GLFW_PRESS;

        sample.axes[static_cast<size_t>(GamepadAxis::LeftX)]        = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        sample.axes[static_cast<size_t>(GamepadAxis::LeftY)]        = -gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        sample.axes[static_cast<size_t>(GamepadAxis::RightX)]       = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
        sample.axes[static_cast<size_t>(GamepadAxis::RightY)]       = -gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        sample.axes[static_cast<size_t>(GamepadAxis::LeftTrigger)]  = (gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
        sample.axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = (gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;

        rx.onGamepadState(rx.userdata, slot, sample);
    }
}


auto Window::GetSize() const -> Extent2D {
    return _impl->target.GetFramebufferExtent();
}

void Window::SetSize(uint32_t width, uint32_t height) noexcept {
    _impl->target.SetFramebufferExtent(width, height);
}

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
    _impl->target.Close();
}

namespace {

void ApplyGlfwCursorMode(GLFWwindow* handle, CursorMode cursorMode, bool mouseCaptured) noexcept {
    if (handle == nullptr) {
        return;
    }
    int glfwMode = GLFW_CURSOR_NORMAL;
    if (mouseCaptured) {
        glfwMode = GLFW_CURSOR_DISABLED;
    } else if (cursorMode == CursorMode::Custom) {
        glfwMode = GLFW_CURSOR_HIDDEN;
    }
    glfwSetInputMode(handle, GLFW_CURSOR, glfwMode);
}

} // namespace

void Window::CaptureMouse(bool captured) {
    _impl->mouseCaptured = captured;
    ApplyGlfwCursorMode(_impl->handle, _impl->cursorMode, _impl->mouseCaptured);
}

auto Window::IsMouseCaptured() const noexcept -> bool {
    return _impl->mouseCaptured;
}

void Window::SetCursorMode(CursorMode mode) {
    _impl->cursorMode = mode;
    ApplyGlfwCursorMode(_impl->handle, _impl->cursorMode, _impl->mouseCaptured);
    if (mode == CursorMode::Custom && !_impl->mouseCaptured && _impl->handle != nullptr && _impl->receiver.onMouseMove != nullptr) {
        double xpos = 0.0;
        double ypos = 0.0;
        glfwGetCursorPos(_impl->handle, &xpos, &ypos);
        _impl->receiver.onMouseMove(_impl->receiver.userdata, static_cast<float>(xpos), static_cast<float>(ypos));
    }
}

auto Window::GetCursorMode() const noexcept -> CursorMode {
    return _impl->cursorMode;
}

auto Window::UsesCustomCursor() const noexcept -> bool {
    return _impl->handle != nullptr && _impl->cursorMode == CursorMode::Custom && !_impl->mouseCaptured;
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
        const char* text = glfwGetClipboardString(_impl->handle);
        return (text != nullptr) ? std::string(text) : std::string();
    }
    return _impl->localClipboard;
}

void Window::SetClipboardText(std::string_view text) {
    _impl->localClipboard.assign(text);
    if (_impl->handle != nullptr) {
        glfwSetClipboardString(_impl->handle, _impl->localClipboard.c_str());
    }
}

}
