// not actual frame per seconds lol. first person shooter.
module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>
export module ZHLN.FPS;

export namespace ZHLN::FPS {

/**
 * @brief Reusable 1D Spring-Mass-Damper Solver.
 * Useful for recoil, land dips, and camera shake.
 */
struct Spring1D {
    float value     = 0.0f;
    float velocity  = 0.0f;
    float stiffness = 200.0f;
    float damping   = 15.0f;

    void Update(float dt, float target = 0.0f) noexcept {
        float force = -stiffness * (value - target) - damping * velocity;
        velocity += force * dt;
        value += velocity * dt;
    }

    void ApplyImpulse(float impulse) noexcept {
        velocity += impulse;
    }
};

/**
 * @brief Reusable 3D Spring-Mass-Damper Solver.
 * Ideal for 3D weapon kick, translational sway, and camera impacts.
 */
struct Spring3D {
    JPH::Vec3 value     = JPH::Vec3::sZero();
    JPH::Vec3 velocity  = JPH::Vec3::sZero();
    float     stiffness = 200.0f;
    float     damping   = 15.0f;

    void Update(float dt, JPH::Vec3Arg target = JPH::Vec3::sZero()) noexcept {
        JPH::Vec3 force = -stiffness * (value - target) - damping * velocity;
        velocity += force * dt;
        value += velocity * dt;
    }

    void ApplyImpulse(JPH::Vec3Arg impulse) noexcept {
        velocity += impulse;
    }
};

/**
 * @brief Parametric Lissajous Camera/Weapon Bobbing.
 */
struct BobEvaluator {
    float speedMultiplier = 1.0f;
    float horizontalScale = 0.045f;
    float verticalScale   = 0.035f;

    [[nodiscard]] JPH::Vec3 Evaluate(float phase, float intensity) const noexcept {
        if (intensity < 0.001f)
            return JPH::Vec3::sZero();

        float x = std::cos(phase * speedMultiplier) * horizontalScale * intensity;
        float y = std::sin(phase * speedMultiplier * 2.0f) * verticalScale * intensity;
        return JPH::Vec3(x, y, 0.0f);
    }
};

/**
 * @brief Generic Viewmodel Sway Solver.
 * Evaluates lag and rotation offsets based on input movement and rotation deltas.
 */
struct SwaySolver {
    float maxSway      = 0.06f;
    float returnSpeed  = 10.0f;
    float currentSwayX = 0.0f;
    float currentSwayY = 0.0f;

    void Update(float dt, float inputDeltaX, float inputDeltaY) noexcept {
        float targetX = std::clamp(-inputDeltaX, -maxSway, maxSway);
        float targetY = std::clamp(-inputDeltaY, -maxSway, maxSway);

        currentSwayX += (targetX - currentSwayX) * std::min(1.0f, returnSpeed * dt);
        currentSwayY += (targetY - currentSwayY) * std::min(1.0f, returnSpeed * dt);
    }
};

} // namespace ZHLN::FPS
