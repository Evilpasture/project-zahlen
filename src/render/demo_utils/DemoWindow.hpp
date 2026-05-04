#pragma once
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace ZHLN::Demo {

struct WindowState {
	uint32_t width = 800;
	uint32_t height = 600;
	bool running = true;
	bool resized = false;

	// Opaque OS handles
	void* os_window = nullptr;
	void* os_instance = nullptr;
	void* metal_layer = nullptr;
};

// Creates the OS Window and fills the state struct
WindowState InitWindow(uint32_t width, uint32_t height, const char* title);

// Pumps the OS message queue
void ProcessEvents(WindowState& state);

// Destroys the OS Window
void DestroyWindow(WindowState& state);

// Returns the OS-specific Vulkan extensions required for the surface
std::vector<const char*> GetRequiredInstanceExtensions();

// Creates the Vulkan Surface from the OS Window
VkSurfaceKHR CreateSurface(VkInstance instance, const WindowState& state);

} // namespace ZHLN::Demo