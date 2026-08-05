// include/Zahlen/Input.hpp
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Types.hpp>
#include <bitset>

namespace ZHLN {

enum class KeyCode : uint8_t {
    Unknown = 0,

    // Numbers (0 - 9)
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    // Alphabet (A - Z)
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    // Function Keys
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    // Modifiers
    LShift,
    RShift,
    LControl,
    RControl,
    LAlt,
    RAlt,

    // Navigation & Editing
    Space,
    Escape,
    Enter,
    Backspace,
    Tab,
    Delete,

    // Arrow Keys
    Up,
    Down,
    Left,
    Right,

    // Mouse Buttons
    LButton,
    RButton,
    MButton
};

struct MouseState {
    float          x = 0, y = 0;
    float          deltaX = 0, deltaY = 0;
    std::bitset<8> buttons;
    float          wheel = 0;
};

class ZHLN_API InputContext {
  public:
    InputContext() = default;

    [[nodiscard]] bool              IsKeyDown(KeyCode key) const noexcept;
    [[nodiscard]] bool              IsMouseButtonDown(KeyCode key) const noexcept;
    [[nodiscard]] const MouseState& GetMouse() const noexcept {
        return _mouse;
    }

    void ResetDeltas();

    [[nodiscard]] bool NeedsResize() const {
        return _needsResize;
    }
    [[nodiscard]] Extent2D GetNewSize() const {
        return _newSize;
    }
    void ClearResizeFlag() {
        _needsResize = false;
    }

    void InjectKeyDown(KeyCode key);
    void InjectKeyUp(KeyCode key);
    void InjectLocalMotion(float x, float y);
    void InjectWheelMotion(float delta);
    void InjectResize(const Extent2D& extent);

  private:
    bool     _needsResize = false;
    Extent2D _newSize {.width = 0, .height = 0};

    // Perfectly sized to the exact number of key enumerators
    std::bitset<ZHLN::Reflect::EnumCount<KeyCode>()> _keys;

    MouseState _mouse;
    float      _lastX = 0, _lastY = 0;
    bool       _firstMouse = true;
};

} // namespace ZHLN
