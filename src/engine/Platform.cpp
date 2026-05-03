#include <Zahlen/Platform.hpp>
#include <Zahlen/Window.hpp>
#include <thread>
#include <chrono>

#ifdef _WIN32
// Use the "plumbing" to get access to Win32 functions safely
#include <Zahlen/detail/Platform.hpp>
#include <LLGL/Window.h>
#pragma comment(lib, "Shcore.lib")
#endif

namespace ZHLN::Platform {

void Init() {
#ifdef _WIN32
    // Force the process to be DPI aware - Fixes the "Small Viewport" issue
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
}

void FocusWindow(Window& window) {
#ifdef _WIN32
    auto* native = static_cast<LLGL::Window*>(window.GetNativeHandle());
    LLGL::WindowDescriptor desc;
    native->GetNativeHandle(&desc, sizeof(desc));
    HWND hwnd = (HWND)desc.windowContext;
    if (hwnd) {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
#endif
}

float GetDisplayScale(Window& window) {
#ifdef _WIN32
    auto* native = static_cast<LLGL::Window*>(window.GetNativeHandle());
    LLGL::WindowDescriptor desc;
    native->GetNativeHandle(&desc, sizeof(desc));
    HWND hwnd = (HWND)desc.windowContext;
    return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
#else
    return 1.0f;
#endif
}

void Sleep(uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

} // namespace ZHLN::Platform