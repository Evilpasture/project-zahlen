#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Types.hpp>
#include <bitset>

namespace ZHLN {

enum class KeyCode : uint8_t { Unknown = 0, W, A, S, D, LShift, RButton, Space, Escape, MaxKeys };

struct MouseState {
	float x = 0, y = 0;
	float deltaX = 0, deltaY = 0;
	std::bitset<8> buttons;
	float wheel = 0;
};

class ZHLN_API InputContext {
  public:
	InputContext() = default;

	[[nodiscard]] bool IsKeyDown(KeyCode key) const noexcept {
		return _keys[static_cast<size_t>(key)];
	}
	[[nodiscard]] bool IsMouseButtonDown(KeyCode key) const noexcept {
		return _keys[static_cast<size_t>(key)];
	}
	[[nodiscard]] const MouseState& GetMouse() const noexcept { return _mouse; }

	void ResetDeltas();

	[[nodiscard]] bool NeedsResize() const { return _needsResize; }
	[[nodiscard]] Extent2D GetNewSize() const { return _newSize; }
	void ClearResizeFlag() { _needsResize = false; }

	// --- Injection API (Called by the hidden OS Window implementation) ---
	void InjectKeyDown(KeyCode key);
	void InjectKeyUp(KeyCode key);
	void InjectLocalMotion(float x, float y);
	void InjectWheelMotion(float delta);
	void InjectResize(const Extent2D& extent);

  private:
	bool _needsResize = false;
	Extent2D _newSize{.width = 0, .height = 0};
	std::bitset<256> _keys;
	MouseState _mouse;
	float _lastX = 0, _lastY = 0;
	bool _firstMouse = true;
};

} // namespace ZHLN
