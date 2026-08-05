// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Platform.hpp"
#include "TTYBackend.hpp"
#include "WindowInternal.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <variant>

namespace ZHLN {

namespace {

// 1. Consteval mapping (Natural forward direction)
[[maybe_unused]] consteval int KeyCodeToGLFW(KeyCode key) noexcept {
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

// 2. C++26 Reflection generates the inverted O(1) lookup table at compile time!
consteval auto BuildGLFWToKeyCodeTable() noexcept {
    std::array<KeyCode, GLFW_KEY_LAST + 1> table {};
    table.fill(KeyCode::Unknown);

    ZHLN::Reflect::ForEachEnumerator<KeyCode>([&]<KeyCode Key>() {
        static constexpr int glfwCode = KeyCodeToGLFW(Key);
        if constexpr (glfwCode >= 0 && glfwCode <= GLFW_KEY_LAST) {
            table[glfwCode] = Key;
        }
    });

    return table;
}

// 3. Runtime function: Single instruction array access (O(1) / Branchless)
KeyCode MapGLFWKey(int key) noexcept {
    static constexpr auto Table = BuildGLFWToKeyCodeTable();
    if (key >= 0 && key <= GLFW_KEY_LAST) [[likely]] {
        return Table[key];
    }
    return KeyCode::Unknown;
}
} // namespace

Window::Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen, InputContext* input, bool useTTY): _impl(std::make_unique<Impl>()) {
    _impl->input  = input;
    _impl->is_tty = useTTY;
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

        if (input != nullptr) {
            glfwSetKeyCallback(_impl->handle, [](GLFWwindow* win, int key, [[maybe_unused]] int scancode, int action, [[maybe_unused]] int mods) {
                auto*   self   = static_cast<Window*>(glfwGetWindowUserPointer(win));
                KeyCode mapped = MapGLFWKey(key);
                if (action == GLFW_PRESS) {
                    self->_impl->input->InjectKeyDown(mapped);
                } else if (action == GLFW_RELEASE) {
                    self->_impl->input->InjectKeyUp(mapped);
                }

                if (action == GLFW_PRESS || action == GLFW_REPEAT) {
                    if (auto* engine = GetEngineContext()) {
                        auto& reg = engine->GetRegistry();
                        for (Entity e: reg.GetEntitiesWith<Components::UITextInputComponent>()) {
                            auto* inputComp = reg.Get<Components::UITextInputComponent>(e);
                            if (inputComp && inputComp->isFocused) {
                                std::string_view curr = inputComp->text;

                                if (key == GLFW_KEY_BACKSPACE && inputComp->cursorIndex > 0) {
                                    std::string next = std::string(curr.substr(0, inputComp->cursorIndex - 1)) +
                                                       std::string(curr.substr(inputComp->cursorIndex));
                                    inputComp->text.assign(next);
                                    inputComp->cursorIndex--;
                                } else if (key == GLFW_KEY_LEFT && inputComp->cursorIndex > 0) {
                                    inputComp->cursorIndex--;
                                } else if (key == GLFW_KEY_RIGHT && inputComp->cursorIndex < curr.size()) {
                                    inputComp->cursorIndex++;
                                }
                            }
                        }
                    }
                }
            });

            glfwSetMouseButtonCallback(_impl->handle, [](GLFWwindow* win, int button, int action, [[maybe_unused]] int mods) {
                auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
                if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                    if (action == GLFW_PRESS) {
                        self->_impl->input->InjectKeyDown(KeyCode::RButton);
                    } else if (action == GLFW_RELEASE) {
                        self->_impl->input->InjectKeyUp(KeyCode::RButton);
                    }
                } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
                    if (action == GLFW_PRESS) {
                        self->_impl->input->InjectKeyDown(KeyCode::LButton);
                    } else if (action == GLFW_RELEASE) {
                        self->_impl->input->InjectKeyUp(KeyCode::LButton);
                    }
                }
            });

            glfwSetCursorPosCallback(_impl->handle, [](GLFWwindow* win, double xpos, double ypos) {
                auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));

                int winWidth  = 0;
                int winHeight = 0;
                glfwGetWindowSize(win, &winWidth, &winHeight);

                int fbWidth  = 0;
                int fbHeight = 0;
                glfwGetFramebufferSize(win, &fbWidth, &fbHeight);

                float scaleX = (winWidth > 0) ? (float) fbWidth / (float) winWidth : 1.0f;
                float scaleY = (winHeight > 0) ? (float) fbHeight / (float) winHeight : 1.0f;

                self->_impl->input->InjectLocalMotion(static_cast<float>(xpos) * scaleX, static_cast<float>(ypos) * scaleY);
            });

            glfwSetFramebufferSizeCallback(_impl->handle, [](GLFWwindow* win, int width, int height) {
                auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
                self->_impl->input->InjectResize({.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)});
            });

            glfwSetScrollCallback(_impl->handle, [](GLFWwindow* win, [[maybe_unused]] double xoffset, double yoffset) {
                auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
                self->_impl->input->InjectWheelMotion(static_cast<float>(yoffset));
            });

            glfwSetCharCallback(_impl->handle, []([[maybe_unused]] GLFWwindow* win, unsigned int codepoint) {
                if (auto* engine = GetEngineContext()) {
                    auto& reg = engine->GetRegistry();
                    for (Entity e: reg.GetEntitiesWith<Components::UITextInputComponent>()) {
                        auto* inputComp = reg.Get<Components::UITextInputComponent>(e);
                        if (inputComp && inputComp->isFocused) {
                            if (codepoint >= 32 && codepoint <= 126 && inputComp->text.size() < 255) {
                                std::string_view curr = inputComp->text;
                                std::string      next = std::string(curr.substr(0, inputComp->cursorIndex)) + static_cast<char>(codepoint) +
                                                        std::string(curr.substr(inputComp->cursorIndex));
                                inputComp->text.assign(next);
                                inputComp->cursorIndex++;
                            }
                        }
                    }
                }
            });
        }
    }
}

Window::~Window() {
    if (_impl->is_tty) {
        TTYBackend::Shutdown(_impl->tty_context);
    } else if (_impl->handle != nullptr) {
        glfwDestroyWindow(_impl->handle);
    }
}

bool Window::IsRunning() const {
    if (_impl->is_tty) {
        return TTYBackend::IsRunning(_impl->tty_context);
    }
    return glfwWindowShouldClose(_impl->handle) == 0;
}

void Window::ProcessEvents() {
}

Extent2D Window::GetSize() const {
    if (_impl->is_tty) {
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
    glfwFocusWindow(_impl->handle);
}

void* Window::GetNativeHandle() const {
    return _impl->handle;
}

void Window::Close() {
    glfwSetWindowShouldClose(_impl->handle, GLFW_TRUE);
}

void Window::CaptureMouse(bool captured) {
    if (!_impl->is_tty && (_impl->handle != nullptr)) {
        glfwSetInputMode(_impl->handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
}

bool Window::IsTTY() const {
    return _impl->is_tty;
}
void* Window::GetTTYContext() const {
    return _impl->tty_context;
}

bool Window::ReinitTTY() {
    if (_impl->is_tty && _impl->tty_context == nullptr) {
        _impl->tty_context = TTYBackend::Init(_impl->width, _impl->height);
        return _impl->tty_context != nullptr;
    }
    return false;
}

} // namespace ZHLN
