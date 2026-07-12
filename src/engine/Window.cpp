// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Platform.hpp"
#include "TTYBackend.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "ecs/ECS.hpp"
#include <GLFW/glfw3.h>
#include <Rendering.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Window.hpp>
#include <variant>

namespace ZHLN {

static KeyCode MapGLFWKey(int key) {
    switch (key) {
        case GLFW_KEY_W:
            return KeyCode::W;
        case GLFW_KEY_A:
            return KeyCode::A;
        case GLFW_KEY_S:
            return KeyCode::S;
        case GLFW_KEY_D:
            return KeyCode::D;
        case GLFW_KEY_LEFT_SHIFT:
            return KeyCode::LShift;
        case GLFW_KEY_SPACE:
            return KeyCode::Space;
        case GLFW_KEY_ESCAPE:
            return KeyCode::Escape;
        case GLFW_KEY_R:
            return KeyCode::R;
        case GLFW_KEY_E:
            return KeyCode::E;
        default:
            return KeyCode::Unknown;
    }
}

struct Window::Impl {
    GLFWwindow*   handle      = nullptr;
    InputContext* input       = nullptr;
    bool          is_tty      = false;
    void*         tty_context = nullptr;
    uint32_t      width       = 0;
    uint32_t      height      = 0;
};

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
                // Match monitor's native video configurations
                glfwWindowHint(GLFW_RED_BITS, mode->redBits);
                glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
                glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
                glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

                // Automatically scale the resolution if no dimensions are supplied
                width  = (width == 0) ? static_cast<uint32_t>(mode->width) : width;
                height = (height == 0) ? static_cast<uint32_t>(mode->height) : height;
            }
        }

        _impl->handle = glfwCreateWindow(width, height, title.c_str(), monitor, nullptr);
        glfwSetWindowUserPointer(_impl->handle, this);

        if (_impl->handle != nullptr) {
            // Force the window manager to map and display the window immediately
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

                // Query both the virtual window size (points) and physical framebuffer size
                // (pixels)
                int winWidth  = 0;
                int winHeight = 0;
                glfwGetWindowSize(win, &winWidth, &winHeight);

                int fbWidth  = 0;
                int fbHeight = 0;
                glfwGetFramebufferSize(win, &fbWidth, &fbHeight);

                // Calculate the High-DPI / Retina scale factors
                float scaleX = (winWidth > 0) ? (float) fbWidth / (float) winWidth : 1.0f;
                float scaleY = (winHeight > 0) ? (float) fbHeight / (float) winHeight : 1.0f;

                // Inject the adjusted pixel-pace coordinates into the input system
                self->_impl->input->InjectLocalMotion(static_cast<float>(xpos) * scaleX, static_cast<float>(ypos) * scaleY);
            });

            glfwSetFramebufferSizeCallback(_impl->handle, [](GLFWwindow* win, int width, int height) {
                auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
                self->_impl->input->InjectResize({.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)});
            });

            glfwSetScrollCallback(_impl->handle, [](GLFWwindow* win, double xoffset, double yoffset) {
                auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
                self->_impl->input->InjectWheelMotion(static_cast<float>(yoffset));
            });

            // Handle control keys (Backspace / Arrow keys) for focused text inputs
            glfwSetKeyCallback(_impl->handle, [](GLFWwindow* win, int key, [[maybe_unused]] int scancode, int action, [[maybe_unused]] int mods) {
                auto*   self   = static_cast<Window*>(glfwGetWindowUserPointer(win));
                KeyCode mapped = MapGLFWKey(key);
                if (action == GLFW_PRESS) {
                    self->_impl->input->InjectKeyDown(mapped);
                } else if (action == GLFW_RELEASE) {
                    self->_impl->input->InjectKeyUp(mapped);
                }

                // Process text control actions on key press
                if (action == GLFW_PRESS || action == GLFW_REPEAT) {
                    if (auto* engine = GetEngineContext()) {
                        auto& reg = engine->GetRegistry();
                        for (Entity e: reg.GetEntitiesWith<Components::UITextInputComponent>()) {
                            auto* input = reg.Get<Components::UITextInputComponent>(e);
                            if (input && input->isFocused) {
                                std::string_view curr = input->text;

                                if (key == GLFW_KEY_BACKSPACE && input->cursorIndex > 0) {
                                    // Erase character before cursor
                                    std::string next = std::string(curr.substr(0, input->cursorIndex - 1)) + std::string(curr.substr(input->cursorIndex));
                                    input->text.assign(next);
                                    input->cursorIndex--;

                                    // Force immediate redraw
                                    if (auto* text = reg.Get<Components::TextComponent>(e)) {
                                        text->mesh.posBuffer = BufferHandle::Invalid;
                                    }
                                } else if (key == GLFW_KEY_LEFT && input->cursorIndex > 0) {
                                    input->cursorIndex--;
                                } else if (key == GLFW_KEY_RIGHT && input->cursorIndex < curr.size()) {
                                    input->cursorIndex++;
                                }
                            }
                        }
                    }
                }
            });

            // Handle printable character inputs
            glfwSetCharCallback(_impl->handle, [](GLFWwindow* win, unsigned int codepoint) {
                if (auto* engine = GetEngineContext()) {
                    auto& reg = engine->GetRegistry();
                    for (Entity e: reg.GetEntitiesWith<Components::UITextInputComponent>()) {
                        auto* input = reg.Get<Components::UITextInputComponent>(e);
                        if (input && input->isFocused) {
                            // Append only printable ASCII characters within buffer limits
                            if (codepoint >= 32 && codepoint <= 126 && input->text.size() < 255) {
                                std::string_view curr = input->text;
                                std::string      next = std::string(curr.substr(0, input->cursorIndex)) + static_cast<char>(codepoint) +
                                                        std::string(curr.substr(input->cursorIndex));
                                input->text.assign(next);
                                input->cursorIndex++;

                                // Force immediate redraw
                                if (auto* text = reg.Get<Components::TextComponent>(e)) {
                                    text->mesh.posBuffer = BufferHandle::Invalid;
                                }
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
    // Engine::ProcessEvents handles glfwPollEvents now
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
    // We can just use GLFW's built-in focus!
    glfwFocusWindow(_impl->handle);
}

void* Window::GetNativeHandle() const {
    return _impl->handle; // Return GLFWwindow*
}

void Window::Close() {
    glfwSetWindowShouldClose(_impl->handle, GLFW_TRUE);
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

std::expected<void*, Error> Window::CreateVulkanSurface(void* instance, void* physicalDevice, int& outWidth, int& outHeight) noexcept {
    auto logger = [](const char* msg) { ZHLN::Log("{}", msg); };

    auto displaySelector = [](std::span<const VkDisplayPropertiesKHR> displays) -> VkDisplayPropertiesKHR { return displays[0]; };

    auto modeSelector = [](std::span<const VkDisplayModePropertiesKHR> modes) -> VkDisplayModePropertiesKHR { return modes[0]; };

    auto planeSelector = [](std::span<const VkDisplayPlanePropertiesKHR> planeProps, VkDisplayKHR targetDisplay, auto&& getSupported) -> uint32_t {
        auto planeCount = static_cast<uint32_t>(planeProps.size());
        for (uint32_t i = 0; i < planeCount; i++) {
            if (planeProps[i].currentDisplay != VK_NULL_HANDLE && planeProps[i].currentDisplay != targetDisplay) {
                continue;
            }
            std::vector<VkDisplayKHR> supported = getSupported(i);
            for (auto* d: supported) {
                if (d == targetDisplay) {
                    return i;
                }
            }
        }
        return UINT32_MAX;
    };

    auto alphaSelector = [](const VkDisplayPlaneCapabilitiesKHR& planeCaps) -> VkDisplayPlaneAlphaFlagBitsKHR {
        VkDisplayPlaneAlphaFlagBitsKHR alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        if (!(planeCaps.supportedAlpha & alphaMode)) {
            if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR) {
                alphaMode = VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;
            } else if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR) {
                alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR;
            } else if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR) {
                alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR;
            }
        }
        return alphaMode;
    };

    auto windowCreate = [this](VkInstance inst, uint32_t& w, uint32_t& h) -> std::expected<VkSurfaceKHR, Error> {
        if (glfwVulkanSupported() == GLFW_FALSE) {
            return std::unexpected(SurfaceCreationError::WindowSurfaceUnsupported);
        }
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult     err     = glfwCreateWindowSurface(inst, _impl->handle, nullptr, &surface);
        auto         check   = Vk::CheckResult(err, "Failed to create GLFW Vulkan window surface");
        if (!check) {
            return std::unexpected(SurfaceCreationError::GLFWSurfaceCreationFailed);
        }
        int f_w = 0;
        int f_h = 0;
        glfwGetFramebufferSize(_impl->handle, &f_w, &f_h);
        w = static_cast<uint32_t>(f_w);
        h = static_cast<uint32_t>(f_h);
        return surface;
    };

    uint32_t hwWidth  = 0;
    uint32_t hwHeight = 0;

    // Define a type-erased variant representing our possible configurations
    using ConfigVariant = std::variant<
        ZHLN::Vk::WindowedConfig<decltype(windowCreate)>,
        ZHLN::Vk::TTYConfig<decltype(logger), decltype(displaySelector), decltype(modeSelector), decltype(planeSelector), decltype(alphaSelector)>>;

    // Use an Immediately Invoked Function Expression (IIFE) to initialize the variant directly,
    // bypassing the default-constructibility constraint of the first alternative.
    const auto config = [&]() -> ConfigVariant {
        if (_impl->is_tty) {
            return ZHLN::Vk::TTYConfig {
                .physicalDevice = static_cast<VkPhysicalDevice>(physicalDevice),
                .log            = std::move(logger),
                .selectDisplay  = std::move(displaySelector),
                .selectMode     = std::move(modeSelector),
                .selectPlane    = std::move(planeSelector),
                .selectAlpha    = std::move(alphaSelector)
            };
        }
        return ZHLN::Vk::WindowedConfig {.windowCreate = std::move(windowCreate)};
    }();

    auto surface_res = ZHLN::Vk::Surface::Create(static_cast<VkInstance>(instance), hwWidth, hwHeight, config);

    if (!surface_res) {
        return std::unexpected(surface_res.error());
    }

    outWidth      = static_cast<int>(hwWidth);
    outHeight     = static_cast<int>(hwHeight);
    _impl->width  = hwWidth;
    _impl->height = hwHeight;

    return static_cast<void*>(surface_res->Release());
}

} // namespace ZHLN
