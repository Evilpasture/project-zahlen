#include "Platform.hpp"

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
};

Window::Window(const String32& title, uint32_t width, uint32_t height, InputContext* input)
	: _impl(std::make_unique<Impl>()) {

	_impl->input = input;

	// Tell GLFW NOT to create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	_impl->handle = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);

	// Store 'this' pointer in GLFW so callbacks can access the InputContext
	glfwSetWindowUserPointer(_impl->handle, this);

	if (input) {
		glfwSetKeyCallback(_impl->handle,
						   [](GLFWwindow* win, int key, [[maybe_unused]] int scancode, int action,
							  [[maybe_unused]] int mods) {
							   auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
							   KeyCode mapped = MapGLFWKey(key);
							   if (action == GLFW_PRESS)
								   self->_impl->input->InjectKeyDown(mapped);
							   else if (action == GLFW_RELEASE)
								   self->_impl->input->InjectKeyUp(mapped);
						   });

		glfwSetMouseButtonCallback(
			_impl->handle, [](GLFWwindow* win, int button, int action, [[maybe_unused]] int mods) {
				auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
				if (button == GLFW_MOUSE_BUTTON_RIGHT) {
					if (action == GLFW_PRESS)
						self->_impl->input->InjectKeyDown(KeyCode::RButton);
					else if (action == GLFW_RELEASE)
						self->_impl->input->InjectKeyUp(KeyCode::RButton);
				}
			});

		glfwSetCursorPosCallback(_impl->handle, [](GLFWwindow* win, double xpos, double ypos) {
			auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			self->_impl->input->InjectLocalMotion(static_cast<float>(xpos),
												  static_cast<float>(ypos));
		});

		glfwSetFramebufferSizeCallback(_impl->handle, [](GLFWwindow* win, int width, int height) {
			auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			self->_impl->input->InjectResize(
				{static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
		});

		glfwSetScrollCallback(_impl->handle, [](GLFWwindow* win, double xoffset, double yoffset) {
			auto* self = static_cast<Window*>(glfwGetWindowUserPointer(win));
			self->_impl->input->InjectWheelMotion(static_cast<float>(yoffset));
		});
	}
}

Window::~Window() {
	if (_impl->handle) {
		glfwDestroyWindow(_impl->handle);
	}
}

bool Window::IsRunning() const {
	return !glfwWindowShouldClose(_impl->handle);
}

void Window::ProcessEvents() {
	// Engine::ProcessEvents handles glfwPollEvents now
}

Extent2D Window::GetSize() const {
	int w, h;
	// glfwGetWindowSize returns logical points
	// glfwGetFramebufferSize returns actual pixels
	glfwGetFramebufferSize(_impl->handle, &w, &h);
	return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
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

} // namespace ZHLN
