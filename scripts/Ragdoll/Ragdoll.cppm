module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
export module ZHLN.Ragdoll;
import ZHLN.Rig;

export namespace ZHLN::Physics {

struct LinkConstraint {
    uint32_t indexA;
    uint32_t indexB;
    float    restLength;
    float    stiffness = 1.0f;
    float    minLimit  = -1.0f; // Ignored if negative
    float    maxLimit  = -1.0f;
};

/**
 * @brief Reusable Verlet Particle and Constraint Solver.
 */
class VerletSolver {
  public:
    std::vector<JPH::Vec3> positions;
    std::vector<JPH::Vec3> previousPositions;
    std::vector<float>     massInv;

    VerletSolver() = default;

    void AddParticle(JPH::Vec3Arg pos, float mass) {
        positions.push_back(pos);
        previousPositions.push_back(pos);
        massInv.push_back(mass > 0.0f ? 1.0f / mass : 0.0f);
    }

    void AddConstraint(uint32_t a, uint32_t b, float stiffness, float minLimit = -1.0f, float maxLimit = -1.0f) {
        if (a >= positions.size() || b >= positions.size())
            return;
        float rest = (positions[b] - positions[a]).Length();
        m_constraints.push_back({a, b, rest, stiffness, minLimit, maxLimit});
    }

    void ApplyImpulse(uint32_t index, JPH::Vec3Arg force) {
        if (index < previousPositions.size()) {
            previousPositions[index] -= force * massInv[index];
        }
    }

    void Step(float dt, float gravity, float damping, int iterations, const std::function<void(JPH::Vec3&, float)>& collisionResolver = nullptr) {
        size_t    count = positions.size();
        JPH::Vec3 gravityVec(0.0f, gravity, 0.0f);

        // 1. Integrate
        for (size_t i = 0; i < count; ++i) {
            if (massInv[i] == 0.0f)
                continue;

            JPH::Vec3& p  = positions[i];
            JPH::Vec3& pr = previousPositions[i];

            JPH::Vec3 vel = (p - pr) * damping;
            pr            = p;
            p             = p + vel + gravityVec * (dt * dt);
        }

        // 2. Solve Constraints
        for (int it = 0; it < iterations; ++it) {
            for (const auto& con: m_constraints) {
                JPH::Vec3& pa   = positions[con.indexA];
                JPH::Vec3& pb   = positions[con.indexB];
                float      wA   = massInv[con.indexA];
                float      wB   = massInv[con.indexB];
                float      wSum = wA + wB;
                if (wSum <= 0.0f)
                    continue;

                JPH::Vec3 v = pb - pa;
                float     d = v.Length();
                if (d < 1e-6f)
                    continue;

                // Realigned property mapping to use restLength
                float target = con.restLength;
                if (con.minLimit >= 0.0f) {
                    if (d >= con.minLimit && d <= con.maxLimit)
                        continue;
                    target = (d < con.minLimit) ? con.minLimit : con.maxLimit;
                }

                float diff = ((d - target) / d) / wSum * con.stiffness;
                pa += v * (wA * diff);
                pb -= v * (wB * diff);
            }

            // 3. Resolve Collisions
            if (collisionResolver) {
                for (size_t i = 0; i < count; ++i) {
                    if (massInv[i] > 0.0f) {
                        collisionResolver(positions[i], massInv[i]);
                    }
                }
            }
        }
    }

    void Clear() {
        positions.clear();
        previousPositions.clear();
        massInv.clear();
        m_constraints.clear();
    }

  private:
    std::vector<LinkConstraint> m_constraints;
};

} // namespace ZHLN::Physics
