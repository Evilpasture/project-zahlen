#pragma once
#include <Zahlen/detail/Platform.hpp>
#include <LLGL/Window.h>
#include <string_view>

namespace ZHLN::Platform {

/**
 * @brief Performs one-time OS initialization (DPI awareness, etc.)
 */
void Init();

/**
 * @brief Brings the specified native window to the foreground.
 */
void FocusWindow(LLGL::Window& window);

/**
 * @brief Returns the physical scale factor (1.0 = 100%, 2.0 = 200%).
 */
float GetDisplayScale(LLGL::Window& window);

/**
 * @brief High-precision sleep (wraps std::this_thread or OS-specific).
 */
void Sleep(uint32_t milliseconds);

} // namespace ZHLN::Platform