// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Platform.hpp"
#include "TTYBackend.hpp"

#include <GLFW/glfw3.h> // NEW
#include <Zahlen/Input.hpp>
#include <Zahlen/Window.hpp>

namespace ZHLN {

static KeyCode MapGLFWKey(int key) {
	switch (key) {
		case GLFW_KEY_W:
			return KeyCode::W;
		case GLFW_KEY_A:
			return KeyCode::A;
		case GLFW_KEY_S:
			return KeyCode::S;
		case GLFW_KEY_D:
			return KeyCode::D;
		case GLFW_KEY_LEFT_SHIFT:
			return KeyCode::LShift;
		case GLFW_KEY_SPACE:
			return KeyCode::Space;
		case GLFW_KEY_ESCAPE:
			return KeyCode::Escape;
		case GLFW_KEY_R:
			return KeyCode::R;
		default:
			return KeyCode::Unknown;
	}
}

struct Window::Impl {
	GLFWwindow* handle = nullptr;
	InputContext* input = nullptr;
	bool is_tty = false;
	void* tty_context = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
};

Window::Window(const String32& title, uint32_t width, uint32_t height, bool fullscreen,
			   InputContext* input, bool useTTY)
	: _impl(std::make_unique<Impl>()) {

	_impl->input = input;
	_impl->is_tty = useTTY;
	if (_impl->is_tty) {
		_impl->width = width;
		_impl->height = height;
		_impl->tty_context = TTYBackend::Init(width, height);
	} else {
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

		GLFWmonitor* monitor = nullptr;
		if (fullscreen) {
			monitor = glfwGetPrimaryMonitor();
			if (monitor != nullptr) {
				const GLFWvidmode* mode = glfwGetVideoMode(monitor);
				// Match monitor's native video configurations
				glfwWindowHint(GLFW_RED_BITS, mode->redBits);
				glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
				glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
				glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

				// Automatically scale the resolution if no dimensions are supplied
				width = (width == 0) ? static_cast<uint32_t>(mode->width) : width;
				height = (height == 0) ? static_cast<uint32_t>(mode->height) : height;
			}
		}

		_impl->handle = glfwCreateWindow(width, height, title.c_str(), monitor, nullptr);
		glfwSetWindowUserPointer(_impl->handle, this);

		if (input != nullptr) {
			glfwSetKeyCallback(_impl->handle,
							   [](GLFWwindow* win, int key, [[maybe_unused]] int scancode,
								  int action, [[maybe_unused]] int mods) {
								   auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
								   KeyCode mapped = MapGLFWKey(key);
								   if (action == GLFW_PRESS) {
									   self->_impl->input->InjectKeyDown(mapped);
								   } else if (action == GLFW_RELEASE) {
									   self->_impl->input->InjectKeyUp(mapped);
								   }
							   });

			glfwSetMouseButtonCallback(_impl->handle, [](GLFWwindow* win, int button, int action,
														 [[maybe_unused]] int mods) {
				auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
				if (button == GLFW_MOUSE_BUTTON_RIGHT) {
					if (action == GLFW_PRESS) {
						self->_impl->input->InjectKeyDown(KeyCode::RButton);
					} else if (action == GLFW_RELEASE) {
						self->_impl->input->InjectKeyUp(KeyCode::RButton);
					}
				}
			});

			glfwSetCursorPosCallback(_impl->handle, [](GLFWwindow* win, double xpos, double ypos) {
				auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
				self->_impl->input->InjectLocalMotion(static_cast<float>(xpos),
													  static_cast<float>(ypos));
			});

			glfwSetFramebufferSizeCallback(
				_impl->handle, [](GLFWwindow* win, int width, int height) {
					auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
					self->_impl->input->InjectResize({.width = static_cast<uint32_t>(width),
													  .height = static_cast<uint32_t>(height)});
				});

			glfwSetScrollCallback(
				_impl->handle, [](GLFWwindow* win, double xoffset, double yoffset) {
					auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
					self->_impl->input->InjectWheelMotion(static_cast<float>(yoffset));
				});
		}
	}
}

Window::~Window() {
	if (_impl->is_tty) {
		TTYBackend::Shutdown(_impl->tty_context);
	} else if (_impl->handle != nullptr) {
		glfwDestroyWindow(_impl->handle);
	}
}

bool Window::IsRunning() const {
	if (_impl->is_tty) {
		return TTYBackend::IsRunning(_impl->tty_context);
	}
	return glfwWindowShouldClose(_impl->handle) == 0;
}

void Window::ProcessEvents() {
	// Engine::ProcessEvents handles glfwPollEvents now
}

Extent2D Window::GetSize() const {
	if (_impl->is_tty) {
		return {.width = _impl->width, .height = _impl->height};
	}

	int w = 0;
	int h = 0;
	glfwGetFramebufferSize(_impl->handle, &w, &h);
	return {.width = static_cast<uint32_t>(w), .height = static_cast<uint32_t>(h)};
}

void Window::SetSize(uint32_t width, uint32_t height) noexcept {
	_impl->width = width;
	_impl->height = height;
}

void Window::Focus() {
	// We can just use GLFW's built-in focus!
	glfwFocusWindow(_impl->handle);
}

void* Window::GetNativeHandle() const {
	return _impl->handle; // Return GLFWwindow*
}

void Window::Close() {
	glfwSetWindowShouldClose(_impl->handle, GLFW_TRUE);
}

bool Window::IsTTY() const {
	return _impl->is_tty;
}
void* Window::GetTTYContext() const {
	return _impl->tty_context;
}

bool Window::ReinitTTY() {
	if (_impl->is_tty && _impl->tty_context == nullptr) {
		_impl->tty_context = TTYBackend::Init(_impl->width, _impl->height);
		return _impl->tty_context != nullptr;
	}
	return false;
}

} // namespace ZHLN
