#ifdef __linux__
#include "DemoWindow.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifndef VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#include <print>

namespace ZHLN::Demo {

WindowState InitWindow(uint32_t width, uint32_t height, const char* title) {
    WindowState state;
    state.width = width;
    state.height = height;
    state.running = true;

    // 1. Open Connection to X Server
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::println(stderr, "Linux: Failed to open X display.");
        state.running = false;
        return state;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    // 2. Create Window
    Window window = XCreateSimpleWindow(
        display, root, 
        0, 0, width, height, 0, 
        BlackPixel(display, screen), 
        WhitePixel(display, screen)
    );

    XStoreName(display, window, title);

    // 3. Request Events
    // ExposureMask: for drawing/resizing
    // StructureNotifyMask: for resize/close
    // PointerMotionMask: for mouse movement
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | PointerMotionMask);

    // 4. Handle Close Button (WM_DELETE_WINDOW)
    Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);

    // 5. Show Window
    XMapWindow(display, window);
    XFlush(display);

    state.os_window = (void*)window;
    state.os_instance = (void*)display;
    return state;
}

void ProcessEvents(WindowState& state) {
    Display* display = (Display*)state.os_instance;
    if (!display) return;

    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);

        switch (event.type) {
            case ClientMessage: {
                // Check if it's the close message
                static Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
                if ((Atom)event.xclient.data.l[0] == wmDeleteMessage) {
                    state.running = false;
                }
                break;
            }
            case ConfigureNotify: {
                // Resize event
                uint32_t new_w = event.xconfigure.width;
                uint32_t new_h = event.xconfigure.height;
                if (new_w != state.width || new_h != state.height) {
                    state.width = new_w;
                    state.height = new_h;
                    state.resized = true;
                }
                break;
            }
            case MotionNotify: {
                state.mouse_x = (float)event.xmotion.x;
                state.mouse_y = (float)event.xmotion.y;
                break;
            }
        }
    }
}

void DestroyWindow(WindowState& state) {
    if (state.os_instance) {
        Display* display = (Display*)state.os_instance;
        XDestroyWindow(display, (Window)state.os_window);
        XCloseDisplay(display);
        state.os_instance = nullptr;
    }
}

std::vector<const char*> GetRequiredInstanceExtensions() {
    return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
}

VkSurfaceKHR CreateSurface(VkInstance instance, const WindowState& state) {
    VkXlibSurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .dpy = (Display*)state.os_instance,
        .window = (Window)state.os_window
    };

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateXlibSurfaceKHR(instance, &info, nullptr, &surface) != VK_SUCCESS) {
        std::println(stderr, "Failed to create Xlib Surface");
    }
    return surface;
}

void UpdateWindowTitle(WindowState& state, const char* title) {
    Display* display = (Display*)state.os_instance;
    XStoreName(display, (Window)state.os_window, title);
}

} // namespace ZHLN::Demo
#endif