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

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ZHLN::Platform {

MappedFile OpenMappedFile(const char* path) {
	MappedFile file;
#if defined(_WIN32)
	HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
							   FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return file;

	LARGE_INTEGER size;
	GetFileSizeEx(hFile, &size);
	file.size = size.QuadPart;

	HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (!hMapping) {
		CloseHandle(hFile);
		return file;
	}

	file.data = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
	file.osHandle = hFile;
	file.osMapping = hMapping;
#else
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return file;
	}

	struct stat sb{};
	if (fstat(fd, &sb) < 0) {
		close(fd);
		return file;
	}
	file.size = sb.st_size;

	file.data = mmap(nullptr, file.size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (file.data == MAP_FAILED) {
		file.data = nullptr;
	}
	file.osHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif
	return file;
}

void CloseMappedFile(MappedFile& file) {
	if (file.data == nullptr) {
		return;
	}
#if defined(_WIN32)
	UnmapViewOfFile(file.data);
	CloseHandle(file.osMapping);
	CloseHandle(file.osHandle);
#else
	munmap(file.data, file.size);
	close(static_cast<int>(reinterpret_cast<intptr_t>(file.osHandle)));
#endif
	file.data = nullptr;
}

void SetHighPriority() {
#ifdef __APPLE__
	// This is the magic command for macOS.
	// QOS_CLASS_USER_INTERACTIVE tells the scheduler to prioritize P-Cores
	// and ignore power-saving E-core migration.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

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
