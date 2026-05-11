#include "Platform.hpp"

#include <Zahlen/Window.hpp>
#include <chrono>
#include <thread>

// 1. Always include the base GLFW (and Vulkan if needed) for all platforms
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
// 2. Win32-specific plumbing (internally handles ifdef logic)
#include <detail/Platform.hpp>

#ifdef _WIN32

// 3. Only expose Win32-specific native access here
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#pragma comment(lib, "Shcore.lib")
#endif

namespace ZHLN::Platform {

void Init() {
#ifdef _WIN32
	// Force the process to be DPI aware - Fixes the "Small Viewport" issue on high-DPI screens
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
}

void FocusWindow(Window& window) {
#ifdef _WIN32
	// Extract the GLFW handle and then the Win32 HWND
	auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
	if (!glfwHandle)
		return;

	HWND hwnd = glfwGetWin32Window(glfwHandle);
	if (hwnd) {
		// Bring to front and grab keyboard focus
		SetForegroundWindow(hwnd);
		SetFocus(hwnd);
	}
#else
	// Fallback for other platforms
	auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
	if (glfwHandle) {
		glfwFocusWindow(glfwHandle);
	}
#endif
}

float GetDisplayScale(Window& window) {
#ifdef _WIN32
	auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
	if (!glfwHandle)
		return 1.0f;

	HWND hwnd = glfwGetWin32Window(glfwHandle);
	// 96 DPI is the standard 100% scale
	return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
#else
	// For non-Windows platforms, GLFW provides content scale
	auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
	if (glfwHandle) {
		float xscale, yscale;
		glfwGetWindowContentScale(glfwHandle, &xscale, &yscale);
		return xscale;
	}
	return 1.0f;
#endif
}

void Sleep(uint32_t milliseconds) {
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

} // namespace ZHLN::Platform