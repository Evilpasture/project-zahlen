// Abstracts OS specifics for you.

#pragma once
#include <cstdint>
#include <detail/Platform.hpp>

namespace ZHLN {
class Window;
}

namespace ZHLN::Platform {

void SetHighPriority();

/**
 * @brief Performs one-time OS initialization (DPI awareness, etc.)
 */
void Init();

/**
 * @brief Brings the specified native window to the foreground.
 */
void FocusWindow(Window& window);

/**
 * @brief Returns the physical scale factor (1.0 = 100%, 2.0 = 200%).
 */
float GetDisplayScale(Window& window);

/**
 * @brief High-precision sleep (wraps std::this_thread or OS-specific).
 */
void Sleep(uint32_t milliseconds);

} // namespace ZHLN::Platform