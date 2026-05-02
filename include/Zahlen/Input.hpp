#pragma once

#include <LLGL/Key.h>
#include <LLGL/Window.h>
#include <Zahlen/detail/Platform.hpp>
#include <bitset>

namespace ZHLN {

struct MouseState {
	float x = 0, y = 0;
	float deltaX = 0, deltaY = 0;
	std::bitset<256> buttons; // Buttons are keys in this version
	float wheel = 0;
};

class InputContext : public LLGL::Window::EventListener {
  public:
	InputContext() = default;

	bool IsKeyDown(LLGL::Key key) const noexcept { return _keys[static_cast<size_t>(key)]; }
	bool IsMouseButtonDown(LLGL::Key key) const noexcept { return _keys[static_cast<size_t>(key)]; }
	const MouseState& GetMouse() const noexcept { return _mouse; }

	void ResetDeltas();

	// --- LLGL Window::EventListener Overrides ---
	void OnKeyDown(LLGL::Window& sender, LLGL::Key key) override;
	void OnKeyUp(LLGL::Window& sender, LLGL::Key key) override;
	void OnLocalMotion(LLGL::Window& sender, const LLGL::Offset2D& position) override;
	void OnWheelMotion(LLGL::Window& sender, int delta) override;

  private:
	std::bitset<256> _keys;
	MouseState _mouse;
	float _lastX = 0, _lastY = 0;
	bool _firstMouse = true;
};

} // namespace ZHLN