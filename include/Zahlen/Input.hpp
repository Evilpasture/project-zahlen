// include/Zahlen/Input.hpp
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Types.hpp>

namespace ZHLN {

namespace ECS {
class Registry;
}

enum class KeyCode : uint8_t {
    Unknown = 0,
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
    LShift,
    RShift,
    LControl,
    RControl,
    LAlt,
    RAlt,
    Space,
    Escape,
    Enter,
    Backspace,
    Tab,
    Delete,
    Up,
    Down,
    Left,
    Right,
    LButton,
    RButton,
    MButton
};

class ZHLN_API InputManager {
  public:
    InputManager() noexcept = default;
    explicit InputManager(ECS::Registry& reg) noexcept: _registry(&reg) {
    }

    // Event Injections (Mutators)
    void InjectKeyDown(KeyCode key) noexcept;
    void InjectKeyUp(KeyCode key) noexcept;
    void InjectLocalMotion(float x, float y) noexcept;
    void InjectWheelMotion(float delta) noexcept;
    void InjectResize(const Extent2D& extent) noexcept;
    void ResetDeltas() noexcept;

    // Convenience State Queries (Readers)
    [[nodiscard]] bool IsKeyDown(KeyCode key) const noexcept;
    [[nodiscard]] bool IsMouseButtonDown(KeyCode key) const noexcept;

    // --- Added Resize Helpers ---
    [[nodiscard]] bool     NeedsResize() const noexcept;
    [[nodiscard]] Extent2D GetNewSize() const noexcept;
    void                   ClearResizeFlag() noexcept;

    [[nodiscard]] float GetMouseX() const noexcept;
    [[nodiscard]] float GetMouseY() const noexcept;
    [[nodiscard]] float GetMouseDeltaX() const noexcept;
    [[nodiscard]] float GetMouseDeltaY() const noexcept;
    [[nodiscard]] float GetMouseWheel() const noexcept;

  private:
    ECS::Registry* _registry = nullptr;
};

} // namespace ZHLN
