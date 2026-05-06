#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include "DemoWindow.hpp"
#include <print>

namespace ZHLN::Demo {

static auto CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    // 1. On creation, grab the pointer passed to CreateWindowEx
    if (msg == WM_NCCREATE) {
        auto* create_struct = reinterpret_cast<CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
    }

    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    
    if (state) {
        switch (msg) {
            case WM_CLOSE:
                state->running = false;
                return 0;

            case WM_SIZE:
                state->width = LOWORD(lp);
                state->height = HIWORD(lp);
                state->resized = true;
                return 0;

            case WM_MOUSEMOVE:
                state->mouse_x = static_cast<float>((short)LOWORD(lp));
                state->mouse_y = static_cast<float>((short)HIWORD(lp));
                return 0;
        }
    }

    // Use DefWindowProcA to match RegisterClassExA
    return DefWindowProcA(hwnd, msg, wp, lp);
}

WindowState InitWindow(uint32_t width, uint32_t height, const char* title) {
    WindowState state;
    state.width = width;
    state.height = height;
    state.running = true;

    HINSTANCE hInstance = GetModuleHandleA(nullptr);
    
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXA wc = {
            .cbSize = sizeof(WNDCLASSEXA),
            .style = CS_OWNDC,
            .lpfnWndProc = WndProc,
            .hInstance = hInstance,
            .hCursor = LoadCursor(nullptr, IDC_ARROW),
            .lpszClassName = "ZHLNDemo",
        };
        RegisterClassExA(&wc);
        registered = true;
    }

    HWND hwnd = CreateWindowExA(
        0, "ZHLNDemo", title, 
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height, 
        nullptr, nullptr, hInstance, 
        &state 
    );

    if (!hwnd) {
        std::println(stderr, "Win32: Failed to create window. Error: {}", GetLastError());
        state.running = false;
        return state;
    }

    state.os_window = hwnd;
    state.os_instance = hInstance;
    return state;
}

void ProcessEvents(WindowState& state) {
    // Lock in the address of the state object from main if it changed due to return-by-value
    HWND hwnd = (HWND)state.os_window;
    if (GetWindowLongPtrA(hwnd, GWLP_USERDATA) != reinterpret_cast<LONG_PTR>(&state)) {
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    }

    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void DestroyWindow(WindowState& state) {
    if (state.os_window) {
        ::DestroyWindow(static_cast<HWND>(state.os_window));
        state.os_window = nullptr;
    }
}

std::vector<const char*> GetRequiredInstanceExtensions() {
	return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
}

VkSurfaceKHR CreateSurface(VkInstance instance, const WindowState& state) {
	VkWin32SurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
										.hinstance = static_cast<HINSTANCE>(state.os_instance),
										.hwnd = static_cast<HWND>(state.os_window)};
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface);
	return surface;
}

void UpdateWindowTitle(ZHLN::Demo::WindowState& win, const char* title) {
	SetWindowTextA((HWND)win.os_window, title);
}

} // namespace ZHLN::Demo
#endif // _WIN32