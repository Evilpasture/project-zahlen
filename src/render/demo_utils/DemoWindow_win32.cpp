#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include "../RenderCore.hpp"
#include "DemoWindow.hpp"

namespace ZHLN::Demo {

static auto CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
	auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (msg == WM_CLOSE) {
		if (state)
			state->running = false;
		return 0;
	}
	if (msg == WM_SIZE && state) {
		state->width = LOWORD(lp);
		state->height = HIWORD(lp);
		state->resized = true;
		return 0;
	}
	if (msg == WM_MOUSEMOVE && state) {
		state->mouse_x = static_cast<float>((short)LOWORD(lp));
		state->mouse_y = static_cast<float>((short)HIWORD(lp));
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

WindowState InitWindow(uint32_t width, uint32_t height, const char* title) {
	WindowState state;
	state.width = width;
	state.height = height;

	HINSTANCE hInstance = GetModuleHandleW(nullptr);
	WNDCLASSEXA wc = {sizeof(WNDCLASSEXA),
					  CS_OWNDC,
					  WndProc,
					  0,
					  0,
					  hInstance,
					  LoadCursor(nullptr, IDC_ARROW),
					  nullptr,
					  nullptr,
					  nullptr,
					  "ZHLNDemo",
					  nullptr};
	RegisterClassExA(&wc);

	HWND hwnd = CreateWindowExA(0, "ZHLNDemo", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
								width, height, nullptr, nullptr, hInstance, nullptr);

	state.os_window = hwnd;
	state.os_instance = hInstance;
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

	return state;
}

void ProcessEvents(WindowState& state) {
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

void DestroyWindow(WindowState& state) {
	if (state.os_window)
		DestroyWindow(static_cast<HWND>(state.os_window));
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
#endif