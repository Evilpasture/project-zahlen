// src/engine/WindowInternal.hpp
#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Zahlen/Input.hpp>
#include <Zahlen/Window.hpp>

namespace ZHLN {
struct Window::Impl {
    GLFWwindow*   handle      = nullptr;
    InputContext* input       = nullptr;
    bool          is_tty      = false;
    void*         tty_context = nullptr;
    uint32_t      width       = 0;
    uint32_t      height      = 0;
};
} // namespace ZHLN
